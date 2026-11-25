#pragma once
#include "FractalCore.hpp"
#include <atomic>
#include <chrono>
#include <thread>

namespace fractal {
struct SeedSet {
    std::array<uint32_t,4> seeds{{197703,50293,87631,310733}};
    std::array<int,4> algorithms{{3,4,8,9}};
    bool operator==(const SeedSet& b) const {return seeds==b.seeds && algorithms==b.algorithms;}
    bool operator!=(const SeedSet& b) const {return !(*this==b);}
};
// Single audio-thread writer, non-realtime readers. No blocking on the writer.
struct AtomicSeedSet {
    std::atomic<uint64_t> sequence{0};
    std::array<std::atomic<uint32_t>,4> seeds;
    std::array<std::atomic<int>,4> algorithms;
    AtomicSeedSet() {SeedSet d;for(int i=0;i<4;++i){seeds[i].store(d.seeds[i]);algorithms[i].store(d.algorithms[i]);}}
    void store(const SeedSet& s) {
        sequence.fetch_add(1,std::memory_order_seq_cst);
        for(int i=0;i<4;++i){seeds[i].store(s.seeds[i]);algorithms[i].store(s.algorithms[i]);}
        sequence.fetch_add(1,std::memory_order_seq_cst);
    }
    bool read(SeedSet& out,uint64_t& version) const {
        const uint64_t before=sequence.load();if(before&1u)return false;
        for(int i=0;i<4;++i){out.seeds[i]=seeds[i].load();out.algorithms[i]=algorithms[i].load();}
        version=sequence.load();return before==version && !(version&1u);
    }
};
// Ownership protocol: the worker alone modifies BankSet storage. Audio only
// reads ACTIVE/FADING slots and changes atomic state flags. Shared_ptr release,
// heap allocation, FFTs and table destruction never occur in the audio callback.
class BankWorker {
public:
    enum State {FREE,BUILDING,READY,ACTIVE,FADING};
    struct Slot {
        std::atomic<int> state{FREE};
        uint64_t version=0;
        BankSet bank;
    };
    AtomicSeedSet request;
    std::array<Slot,3> slots;
    std::atomic<bool> busy{true},failed{false};
private:
    struct CacheEntry {uint32_t seed;int algorithm;std::shared_ptr<const Bank> bank;};
    std::vector<CacheEntry> cache;
    std::atomic<bool> quitting{false};
    std::thread thread;
    int acquireWritable() {
        for(int i=0;i<3;++i){int expected=FREE;if(slots[i].state.compare_exchange_strong(expected,BUILDING))return i;}
        // A queued result may be replaced, but never a bank the voice is reading.
        for(int i=0;i<3;++i){int expected=READY;if(slots[i].state.compare_exchange_strong(expected,BUILDING))return i;}
        return -1;
    }
    std::shared_ptr<const Bank> cached(uint32_t seed,int algorithm) {
        for(auto& e:cache)if(e.seed==seed && e.algorithm==algorithm)return e.bank;
        auto bank=buildBank(seed,algorithm);
        if(cache.size()>=8)cache.erase(cache.begin());
        cache.push_back(CacheEntry{seed,algorithm,bank});return bank;
    }
    void run() {
        uint64_t completed=std::numeric_limits<uint64_t>::max();
        while(!quitting.load()) {
            SeedSet target;uint64_t version=0;
            if(!request.read(target,version) || version==completed) {
                std::this_thread::sleep_for(std::chrono::milliseconds(3));continue;
            }
            int index=acquireWritable();
            if(index<0){std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;}
            busy.store(true);failed.store(false);
            bool stale=false;
            try {
                BankSet next;
                for(int i=0;i<4;++i) {
                    if(quitting.load() || request.sequence.load()!=version){stale=true;break;}
                    next.corners[i]=cached(target.seeds[i],target.algorithms[i]);
                }
                if(!stale && request.sequence.load()==version) {
                    slots[index].bank=std::move(next);slots[index].version=version;
                    slots[index].state.store(READY,std::memory_order_release);
                    completed=version;busy.store(false);
                } else slots[index].state.store(FREE);
            } catch(...) {
                slots[index].state.store(FREE);failed.store(true);busy.store(false);
                completed=version; // Changing a seed requests a retry.
            }
        }
    }
public:
    BankWorker():thread([this]{run();}){}
    ~BankWorker(){quitting.store(true);if(thread.joinable())thread.join();}
    BankWorker(const BankWorker&)=delete;
    BankWorker& operator=(const BankWorker&)=delete;
    int takeReady() {
        const uint64_t wanted=request.sequence.load(std::memory_order_acquire);
        for(int i=0;i<3;++i) {
            if(slots[i].state.load(std::memory_order_acquire)!=READY)continue;
            // Claim before inspecting ordinary metadata: worker may also claim READY.
            int expected=READY;
            if(!slots[i].state.compare_exchange_strong(expected,ACTIVE))continue;
            if(slots[i].version==wanted)return i;
            slots[i].state.store(FREE);
        }
        return -1;
    }
};
// Patch memory contains no strings or allocation in the realtime path.
struct SeedMemory {
    static constexpr int COUNT=16;
    std::array<AtomicSeedSet,COUNT> slots;
    std::array<std::atomic<bool>,COUNT> occupied;
    SeedMemory(){for(auto& o:occupied)o.store(false);}
    void save(int index,const SeedSet& seeds){if(index<0 || index>=COUNT)return;slots[index].store(seeds);occupied[index].store(true);}
    bool recall(int index,SeedSet& out) const {
        if(index<0 || index>=COUNT || !occupied[index].load())return false;
        uint64_t version=0;return slots[index].read(out,version); // bounded, never spins
    }
};
} // namespace fractal
