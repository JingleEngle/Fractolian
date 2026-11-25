#pragma once
#include "BankWorker.hpp"
#include "FractalLive.hpp"
#include <mutex>

namespace fractal { namespace live {
// The source family (seeds + algorithms) and roughness are common to the
// polyphonic oscillator. Every voice reads the same immutable four corners.
struct SourceRequest {
    SeedSet family;
    double roughness = .68;
    bool operator==(const SourceRequest& b) const {
        return family == b.family && roughness == b.roughness;
    }
    bool operator!=(const SourceRequest& b) const { return !(*this == b); }
};

struct AtomicSourceRequest {
    std::atomic<uint64_t> sequence{0};
    std::array<std::atomic<uint32_t>, 4> seeds;
    std::array<std::atomic<int>, 4> algorithms;
    std::atomic<double> roughness{.68};
    AtomicSourceRequest() {
        SeedSet s;
        for (int i = 0; i < 4; ++i) {
            seeds[i].store(s.seeds[i]);
            algorithms[i].store(s.algorithms[i]);
        }
    }
    void store(const SourceRequest& r) {
        sequence.fetch_add(1);
        for (int i = 0; i < 4; ++i) {
            seeds[i].store(r.family.seeds[i]);
            algorithms[i].store(r.family.algorithms[i]);
        }
        roughness.store(r.roughness);
        sequence.fetch_add(1);
    }
    bool read(SourceRequest& r, uint64_t& version) const {
        uint64_t before = sequence.load();
        if (before & 1u) return false;
        for (int i = 0; i < 4; ++i) {
            r.family.seeds[i] = seeds[i].load();
            r.family.algorithms[i] = algorithms[i].load();
        }
        r.roughness = roughness.load();
        version = sequence.load();
        return before == version && !(version & 1u);
    }
};

// Only the worker allocates, frees, or changes shared_ptr ownership.
// The audio callback claims fixed slots with bounded atomics, then uses raw
// pointers. A slot cannot be rewritten while ACTIVE or FADING.
class SourceWorker {
public:
    enum State { FREE, BUILDING, READY, ACTIVE, FADING };
    struct Slot {
        std::atomic<int> state{FREE};
        uint64_t version = 0;
        SourceRequest request;
        Bank bank;
    };
    AtomicSourceRequest request;
    std::array<Slot, 3> slots;
    std::atomic<bool> busy{true}, failed{false};
    std::atomic<uint64_t> builds{0};

private:
    struct Cached {
        uint32_t seed;
        int algorithm;
        double roughness;
        std::shared_ptr<const Corner> corner;
    };
    std::vector<Cached> cache;
    std::atomic<bool> quitting{false};
    // UI access only. Never touched by the audio callback.
    std::mutex previewMutex;
    std::shared_ptr<const Bank> previewBank;
    uint64_t nextRevision = 1;
    std::thread worker;

    int writableSlot() {
        for (int wanted : {FREE, READY}) {
            for (int i = 0; i < 3; ++i) {
                int expected = wanted;
                if (slots[i].state.compare_exchange_strong(expected, BUILDING)) return i;
            }
        }
        return -1;
    }
    std::shared_ptr<const Corner> get(uint32_t seed, int algorithm, double roughness) {
        for (const auto& entry : cache)
            if (entry.seed == seed && entry.algorithm == algorithm && entry.roughness == roughness)
                return entry.corner;
        auto corner = buildCorner(seed, algorithm, roughness);
        if (cache.size() >= 8) cache.erase(cache.begin());
        cache.push_back({seed, algorithm, roughness, corner});
        return corner;
    }
    void run() {
        uint64_t completed = std::numeric_limits<uint64_t>::max();
        while (!quitting.load(std::memory_order_relaxed)) {
            SourceRequest target;
            uint64_t version = 0;
            if (!request.read(target, version) || version == completed) {
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
                continue;
            }
            int index = writableSlot();
            if (index < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            busy.store(true, std::memory_order_relaxed);
            failed.store(false, std::memory_order_relaxed);
            try {
                Bank next;
                bool staleFamily = false;
                for (int i = 0; i < 4; ++i) {
                    SourceRequest latest;
                    uint64_t v;
                    if (quitting.load(std::memory_order_relaxed) ||
                        (request.read(latest, v) && latest.family != target.family)) {
                        staleFamily = true;
                        break;
                    }
                    next.corners[i] = get(target.family.seeds[i], target.family.algorithms[i], target.roughness);
                }
                SourceRequest latest;
                uint64_t v = version;
                if (!staleFamily && request.read(latest, v) && latest.family == target.family) {
                    slots[index].bank = std::move(next);
                    slots[index].bank.revision = nextRevision++;
                    slots[index].request = target;
                    slots[index].version = version;
                    {
                        std::lock_guard<std::mutex> lock(previewMutex);
                        previewBank = std::make_shared<Bank>(slots[index].bank);
                    }
                    slots[index].state.store(READY, std::memory_order_release);
                    builds.fetch_add(1, std::memory_order_relaxed);
                    completed = version;
                    busy.store(v != version, std::memory_order_relaxed);
                    // Immediately handle the newest roughness after publishing
                    // a completed intermediate value; dragging cannot starve audio.
                } else {
                    slots[index].state.store(FREE, std::memory_order_release);
                }
            } catch (...) {
                slots[index].state.store(FREE, std::memory_order_release);
                failed.store(true, std::memory_order_relaxed);
                busy.store(false, std::memory_order_relaxed);
                completed = version;
            }
        }
    }

public:
    SourceWorker() : worker([this] { run(); }) {}
    ~SourceWorker() {
        quitting.store(true, std::memory_order_relaxed);
        if (worker.joinable()) worker.join();
    }
    SourceWorker(const SourceWorker&) = delete;
    SourceWorker& operator=(const SourceWorker&) = delete;
    int takeReady(const SeedSet& wanted,uint64_t afterVersion=std::numeric_limits<uint64_t>::max()) {
        int best=-1;
        for (int i = 0; i < 3; ++i) {
            if (slots[i].state.load(std::memory_order_acquire) != READY) continue;
            int expected = READY;
            if (!slots[i].state.compare_exchange_strong(expected, ACTIVE, std::memory_order_acq_rel)) continue;
            // Metadata is safe only after the slot has been claimed.
            if (slots[i].request.family != wanted ||
                (afterVersion!=std::numeric_limits<uint64_t>::max()&&slots[i].version<=afterVersion)) {
                slots[i].state.store(FREE, std::memory_order_release);
                continue;
            }
            if(best<0||slots[i].version>slots[best].version) {
                if(best>=0)slots[best].state.store(FREE,std::memory_order_release);
                best=i;
            }else slots[i].state.store(FREE,std::memory_order_release);
        }
        // The three fixed slots are inspected once. A fast cached build must
        // never let an older READY result replace a more recent accepted target.
        return best;
    }
    std::shared_ptr<const Bank> preview() {
        std::lock_guard<std::mutex> lock(previewMutex);
        return previewBank;
    }
};
}} // namespace fractal::live
