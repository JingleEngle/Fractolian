#pragma once
#include "plugin.hpp"
#include "SourceWorker.hpp"
#include "PanelPresentation.hpp"
#include "OutputScope.hpp"
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <sstream>

struct FractalXY;
struct ExactQuantity : ParamQuantity {
    std::string getDisplayValueString() override { return string::f("%.5f", getDisplayValue()); }
};
struct TuneQuantity : ParamQuantity {
    std::string getDisplayValueString() override { return string::f("%+.5f", getDisplayValue()); }
};
struct SeedQuantity : ParamQuantity {
    std::string getDisplayValueString() override;
    void setDisplayValueString(std::string value) override;
};
struct GlideQuantity : ParamQuantity {
    float getDisplayValue() override { return static_cast<float>(fractal::glideSeconds(getValue()) * 1000); }
    void setDisplayValue(float value) override { setValue(static_cast<float>(fractal::glideNormalized(value / 1000.))); }
    std::string getDisplayValueString() override { return string::f("%.2f", getDisplayValue()); }
};
struct ZoomQuantity : ParamQuantity {
    float getDisplayValue() override { return std::exp2(getValue()); }
    void setDisplayValue(float value) override { setValue(std::log2(fractal::safe(value, 1, 64, 1))); }
    std::string getDisplayValueString() override { return string::f("%.3f", getDisplayValue()); }
};
struct LevelQuantity : ParamQuantity {
    float getDisplayValue() override { return -60 + 57 * getValue(); }
    void setDisplayValue(float value) override { setValue((value + 60) / 57); }
    std::string getDisplayValueString() override { return string::f("%.1f", getDisplayValue()); }
};

struct FractalXY : Module {
    // Preserve the prototype's Rack parameter/input IDs. Obsolete all-seed and
    // bank CV IDs remain accepted in old patches but have no panel controls.
    enum Params {
        X_PARAM,Y_PARAM,ROUGH_PARAM,SMOOTH_PARAM,TUNE_PARAM,OCTAVE_PARAM,GLIDE_PARAM,LEVEL_PARAM,
        X_ATT,Y_ATT,ROUGH_ATT,SMOOTH_ATT,TUNE_ATT,OCTAVE_ATT,GLIDE_ATT,LEVEL_ATT,
        SEED_A,SEED_B,SEED_C,SEED_D,ALGO_A,ALGO_B,ALGO_C,ALGO_D,
        NEW_A,NEW_B,NEW_C,NEW_D,NEW_ALL,STORE_PARAM,RECALL_PARAM,SLOT_PARAM,
        ZOOM_PARAM,POSITION_PARAM,POSITION_SLEW_PARAM,ZOOM_SLEW_PARAM,
        ZOOM_ATT,POSITION_ATT,POSITION_SLEW_ATT,ZOOM_SLEW_ATT,
        AUTO_LEVEL_PARAM,ZOOM_ENABLED_PARAM,POSITION_MODE_PARAM,NUM_PARAMS
    };
    enum Inputs {
        X_INPUT,Y_INPUT,ROUGH_INPUT,SMOOTH_INPUT,PITCH_INPUT,OCTAVE_INPUT,GLIDE_INPUT,LEVEL_INPUT,
        NEW_A_INPUT,NEW_B_INPUT,NEW_C_INPUT,NEW_D_INPUT,NEW_ALL_INPUT,STORE_INPUT,RECALL_INPUT,
        ZOOM_INPUT,POSITION_INPUT,POSITION_SLEW_INPUT,ZOOM_SLEW_INPUT,NUM_INPUTS
    };
    enum Outputs { AUDIO_OUTPUT,NUM_OUTPUTS };
    enum Lights { NEW_A_LIGHT,NEW_B_LIGHT,NEW_C_LIGHT,NEW_D_LIGHT,STORE_LIGHT,RECALL_LIGHT,BUSY_LIGHT,ERROR_LIGHT,NUM_LIGHTS };
    enum : uint32_t { MAX_EXACT_SEED = 2147483647u };

    // Only these parameters are stored in a tone; trigger buttons and the
    // currently selected memory slot must never be replayed by a recall.
    static const std::array<int,27>& toneIds() {
        static const std::array<int,27> ids{{X_PARAM,Y_PARAM,ROUGH_PARAM,SMOOTH_PARAM,
            TUNE_PARAM,OCTAVE_PARAM,GLIDE_PARAM,LEVEL_PARAM,
            X_ATT,Y_ATT,ROUGH_ATT,SMOOTH_ATT,TUNE_ATT,OCTAVE_ATT,GLIDE_ATT,LEVEL_ATT,
            ZOOM_PARAM,POSITION_PARAM,POSITION_SLEW_PARAM,ZOOM_SLEW_PARAM,
            ZOOM_ATT,POSITION_ATT,POSITION_SLEW_ATT,ZOOM_SLEW_ATT,AUTO_LEVEL_PARAM,ZOOM_ENABLED_PARAM,POSITION_MODE_PARAM}};
        return ids;
    }
    struct Tone {
        fractal::SeedSet seeds;
        std::array<float,27> controls{};
        bool hasControls = false;
    };
    struct AtomicControls {
        std::atomic<unsigned> sequence{0};
        std::array<std::atomic<float>,27> values;
        std::atomic<bool> present{false};
        AtomicControls() { for (auto& v : values) v.store(0); }
        void save(const Tone& t) {
            sequence.fetch_add(1);
            for (int i=0;i<27;++i) values[i].store(t.controls[i]);
            present.store(t.hasControls);
            sequence.fetch_add(1);
        }
        bool read(Tone& t) const {
            unsigned before=sequence.load();
            if(before&1u)return false;
            t.hasControls=present.load();
            for(int i=0;i<27;++i)t.controls[i]=values[i].load();
            return before==sequence.load();
        }
    };

    std::array<fractal::live::Voice,16> voices;
    OutputScope scope;
    fractal::live::SharedFrame sharedFrame;
    fractal::live::SourceWorker builder;
    fractal::SeedMemory memories;
    std::array<AtomicControls,16> savedControls;
    std::array<std::atomic<unsigned>,16> toneVersions;
    fractal::Random randomizer;
    fractal::live::SourceRequest requested;
    std::array<std::atomic<uint32_t>,4> exactSeeds, pendingSeeds;
    std::array<float,4> lastSeedProxy{};
    std::array<fractal::Trigger,7> inputEdges,buttonEdges;
    std::array<double,6> flash{};
    std::array<std::array<double,14>,16> lastControls;
    std::array<fractal::live::Settings,16> targets;
    int active=-1,previous=-1,voiceChannels=0;
    uint32_t divider=0;
    uint32_t telemetryRemaining=1;
    uint64_t processedVoiceSamples=0;
    double bankMix=1,lastSampleRate=0;
    bool outputWasConnected=false;
    std::atomic<float> liveX{.5f},liveY{.5f},liveRough{.68f},liveHz{220},liveGlideMs{20},livePosition{.5f},liveLogZoom{0};
    std::atomic<int> liveChannels{1},forcedChannels{0};
    std::atomic<int> uiStore{-1},uiRecall{-1},noticeCode{0};
    std::atomic<unsigned> uiRandomize{0};
    std::array<std::string,16> names;
    std::mutex namesMutex,importMutex; // UI/serialization only
    struct Import {
        Tone current;
        bool hasCurrent=false;
        std::array<Tone,16> sets;
        std::array<bool,16> valid{};
        int channels=0;
    } incoming;
    std::atomic<bool> importReady{false};

    FractalXY() : randomizer(random::u32()) {
        config(NUM_PARAMS,NUM_INPUTS,NUM_OUTPUTS,NUM_LIGHTS);
        configParam<ExactQuantity>(X_PARAM,0,1,.5,"Morph X");
        configParam<ExactQuantity>(Y_PARAM,0,1,.5,"Morph Y (top to bottom)");
        configParam<ExactQuantity>(ROUGH_PARAM,0,1,.68,"Source roughness (shared by all voices)");
        configParam<ExactQuantity>(SMOOTH_PARAM,0,1,.25,"Smoothing","%",0,100);
        configParam<TuneQuantity>(TUNE_PARAM,-12,12,9,"Tune relative to C"," st");
        getParamQuantity(TUNE_PARAM)->description="Panel shows live channel-1 frequency. Right-click entry is semitones relative to C.";
        configParam(OCTAVE_PARAM,0,8,3,"Octave")->snapEnabled=true;
        configParam<GlideQuantity>(GLIDE_PARAM,0,1,fractal::glideNormalized(.02),"Pitch glide time constant"," ms");
        configParam<LevelQuantity>(LEVEL_PARAM,0,1,42.f/57.f,"Output level"," dB");
        configParam<ZoomQuantity>(ZOOM_PARAM,0,6,0,"Zoom","x");
        configParam<ExactQuantity>(POSITION_PARAM,0,1,.5,"Position");
        configParam<ExactQuantity>(POSITION_SLEW_PARAM,0,5,.08,"Position slew"," ms",0,1000);
        configParam(ZOOM_SLEW_PARAM,0,1,.035,"Zoom slew"," ms",0,1000);
        configSwitch(AUTO_LEVEL_PARAM,0,1,1,"Auto level",{"Off","On"});
        configSwitch(ZOOM_ENABLED_PARAM,0,1,1,"Zoom audition",{"Original at 1x","Zoom enabled"});
        configSwitch(POSITION_MODE_PARAM,0,2,1,"Position motion",{"Scan","Bend","Crossfade"});
        const char* labels[]={"Morph X","Morph Y","Roughness","Smoothing","Pitch","Octave","Glide","Level"};
        for(int i=0;i<8;++i) {
            configParam(X_ATT+i,-1,1,i==4?1:0,std::string(labels[i])+" CV attenuverter","%",0,100);
            configInput(X_INPUT+i,std::string(labels[i])+(i==4?" (polyphonic 1 V/oct)":i==2?" (shared mono source control)":" (polyphonic CV)"));
        }
        const char* added[]={"Zoom","Position","Position slew","Zoom slew"};
        for(int i=0;i<4;++i) {
            configParam(ZOOM_ATT+i,-1,1,0,std::string(added[i])+" CV attenuverter","%",0,100);
            configInput(ZOOM_INPUT+i,std::string(added[i])+" (polyphonic CV)");
        }
        for(int i=0;i<4;++i) {
            exactSeeds[i].store(requested.family.seeds[i]);pendingSeeds[i].store(0);
            lastSeedProxy[i]=static_cast<float>(requested.family.seeds[i]);
            auto* q=configParam<SeedQuantity>(SEED_A+i,1,static_cast<float>(MAX_EXACT_SEED),lastSeedProxy[i],std::string("Seed ")+char('A'+i));
            q->snapEnabled=true;
            q->description="Exact integer 1 to 2147483647. Text entry and JSON preserve every bit. Seeds are shared by all voices.";
            configSwitch(ALGO_A+i,0,fractal::ALGORITHMS-1,requested.family.algorithms[i],std::string("Algorithm ")+char('A'+i),
                std::vector<std::string>(fractal::ALGORITHM_NAMES.begin(),fractal::ALGORITHM_NAMES.end()));
            configButton(NEW_A+i,std::string("New seed ")+char('A'+i));
            configInput(NEW_A_INPUT+i,std::string("Regenerate seed ")+char('A'+i)+" (mono rising edge)");
        }
        configButton(NEW_ALL,"Legacy: regenerate all four seeds");
        configButton(STORE_PARAM,"Store complete tone in selected bank");
        configButton(RECALL_PARAM,"Recall complete tone from selected bank");
        configParam(SLOT_PARAM,0,15,0,"Tone bank","",0,1,1)->snapEnabled=true;
        configInput(NEW_ALL_INPUT,"Legacy: regenerate all seeds");
        configInput(STORE_INPUT,"Legacy: store selected bank");
        configInput(RECALL_INPUT,"Legacy: recall selected bank");
        configOutput(AUDIO_OUTPUT,"Polyphonic fractal audio (1 to 16 channels)");
        configLight(BUSY_LIGHT,"Preparing source in background");
        configLight(ERROR_LIGHT,"Source preparation failed");
        for(auto* q:paramQuantities) { q->smoothEnabled=false;q->randomizeEnabled=false; }
        for(auto& c:lastControls)c.fill(std::numeric_limits<double>::quiet_NaN());
        for(auto& v:toneVersions)v.store(0);
    }

    static uint32_t exactSeed(double value) {
        return static_cast<uint32_t>(std::llround(fractal::safe(value,1,MAX_EXACT_SEED,197703)));
    }
    void setSeed(int i,uint32_t value) {
        value=std::max(1u,std::min(static_cast<uint32_t>(MAX_EXACT_SEED),value));
        exactSeeds[i].store(value,std::memory_order_relaxed);
        lastSeedProxy[i]=static_cast<float>(value);
        params[SEED_A+i].setValue(lastSeedProxy[i]);
    }
    void updateSeedParameters() {
        for(int i=0;i<4;++i) {
            uint32_t pending=pendingSeeds[i].exchange(0,std::memory_order_relaxed);
            if(pending)setSeed(i,pending);
            else if(params[SEED_A+i].getValue()!=lastSeedProxy[i])setSeed(i,exactSeed(params[SEED_A+i].getValue()));
        }
    }
    fractal::SeedSet currentSeeds() {
        fractal::SeedSet s;
        for(int i=0;i<4;++i) {
            s.seeds[i]=exactSeeds[i].load(std::memory_order_relaxed);
            s.algorithms[i]=static_cast<int>(std::llround(fractal::safe(params[ALGO_A+i].getValue(),0,fractal::ALGORITHMS-1)));
        }
        return s;
    }
    void applySeeds(const fractal::SeedSet& s) {
        for(int i=0;i<4;++i) { setSeed(i,s.seeds[i]);params[ALGO_A+i].setValue(s.algorithms[i]); }
    }
    bool selectAlgorithm(int corner,int algorithm) {
        if(corner<0||corner>=4||algorithm<0||algorithm>=fractal::ALGORITHMS)return false;
        auto* q=getParamQuantity(ALGO_A+corner);
        if(q->getValue()==algorithm)return false;
        q->setValue(algorithm);return true;
    }
    Tone currentTone() {
        Tone t;t.seeds=currentSeeds();t.hasControls=true;
        for(int i=0;i<27;++i)t.controls[i]=params[toneIds()[i]].getValue();
        return t;
    }
    void applyTone(const Tone& t) {
        applySeeds(t.seeds);
        if(t.hasControls)for(int i=0;i<27;++i)params[toneIds()[i]].setValue(t.controls[i]);
    }
    int selectedSlot() { return static_cast<int>(std::llround(fractal::safe(params[SLOT_PARAM].getValue(),0,15))); }
    void saveTone(int index,const Tone& tone,bool occupied=true) {
        toneVersions[index].fetch_add(1);
        if(occupied)memories.save(index,tone.seeds);else memories.occupied[index].store(false);
        savedControls[index].save(tone);
        toneVersions[index].fetch_add(1);
    }
    bool recallTone(int index,Tone& tone) {
        unsigned version=toneVersions[index].load();
        if(version&1u)return false;
        bool ok=memories.recall(index,tone.seeds)&&savedControls[index].read(tone);
        return ok&&version==toneVersions[index].load();
    }
    bool triggered(int i,int input,int param) {
        bool a=inputEdges[i].process(inputs[input].getVoltage());
        bool b=buttonEdges[i].process(params[param].getValue()*10.);
        return a||b;
    }
    // At full depth a bipolar +/-5 V signal spans the centered XY pad.
    static constexpr double MORPH_PER_VOLT = .1;
    double mod(int parameter,int input,int atten,int channel,double unitsPerVolt=.2) {
        double cv=inputs[input].getPolyVoltage(channel);
        return params[parameter].getValue()+fractal::finite(cv,0)*params[atten].getValue()*unitsPerVolt;
    }
    int channels() {
        int forced=forcedChannels.load(std::memory_order_relaxed);
        if(forced>0)return std::min(16,forced);
        int n=1;
        const int ids[]={X_INPUT,Y_INPUT,SMOOTH_INPUT,PITCH_INPUT,OCTAVE_INPUT,GLIDE_INPUT,LEVEL_INPUT,
                         ZOOM_INPUT,POSITION_INPUT,POSITION_SLEW_INPUT,ZOOM_SLEW_INPUT};
        for(int id:ids)n=std::max(n,inputs[id].getChannels());
        return std::min(n,16);
    }
    std::array<double,14> controlValues(int c) {
        return {{
            mod(X_PARAM,X_INPUT,X_ATT,c,MORPH_PER_VOLT),mod(Y_PARAM,Y_INPUT,Y_ATT,c,MORPH_PER_VOLT),
            mod(SMOOTH_PARAM,SMOOTH_INPUT,SMOOTH_ATT,c),
            params[TUNE_PARAM].getValue()/12.+std::round(fractal::safe(params[OCTAVE_PARAM].getValue()+
                fractal::finite(inputs[OCTAVE_INPUT].getPolyVoltage(c),0)*params[OCTAVE_ATT].getValue(),0,8,3))-4+
                fractal::finite(inputs[PITCH_INPUT].getPolyVoltage(c),0)*params[TUNE_ATT].getValue(),
            mod(GLIDE_PARAM,GLIDE_INPUT,GLIDE_ATT,c),mod(LEVEL_PARAM,LEVEL_INPUT,LEVEL_ATT,c),
            params[ZOOM_PARAM].getValue()+fractal::finite(inputs[ZOOM_INPUT].getPolyVoltage(c),0)*params[ZOOM_ATT].getValue()*1.2,
            mod(POSITION_PARAM,POSITION_INPUT,POSITION_ATT,c),
            mod(POSITION_SLEW_PARAM,POSITION_SLEW_INPUT,POSITION_SLEW_ATT,c),
            mod(ZOOM_SLEW_PARAM,ZOOM_SLEW_INPUT,ZOOM_SLEW_ATT,c),
            params[AUTO_LEVEL_PARAM].getValue(),params[ZOOM_ENABLED_PARAM].getValue(),lastSampleRate,params[POSITION_MODE_PARAM].getValue()}};
    }
    uint32_t polyControls() {
        const int ports[]={X_INPUT,Y_INPUT,SMOOTH_INPUT,PITCH_INPUT,GLIDE_INPUT,LEVEL_INPUT,
                           ZOOM_INPUT,POSITION_INPUT,POSITION_SLEW_INPUT,ZOOM_SLEW_INPUT};
        const int atten[]={X_ATT,Y_ATT,SMOOTH_ATT,TUNE_ATT,GLIDE_ATT,LEVEL_ATT,
                           ZOOM_ATT,POSITION_ATT,POSITION_SLEW_ATT,ZOOM_SLEW_ATT};
        uint32_t mask=0;
        for(int i=0;i<10;++i)if(inputs[ports[i]].getChannels()>1&&params[atten[i]].getValue()!=0)mask|=1u<<i;
        if(inputs[OCTAVE_INPUT].getChannels()>1&&params[OCTAVE_ATT].getValue()!=0)mask|=1u<<3;
        return mask;
    }
    void updateTarget(int c,bool reset,const std::array<double,14>* common=nullptr,uint32_t polyMask=0) {
        std::array<double,14> raw=common?*common:controlValues(c);
        // Decode mono controls once per module sample. Only actual polyphonic
        // control fields need channel-specific arithmetic (including rounding).
        if(common&&c>0) {
            if(polyMask&(1u<<0))raw[0]=mod(X_PARAM,X_INPUT,X_ATT,c,MORPH_PER_VOLT);
            if(polyMask&(1u<<1))raw[1]=mod(Y_PARAM,Y_INPUT,Y_ATT,c,MORPH_PER_VOLT);
            if(polyMask&(1u<<2))raw[2]=mod(SMOOTH_PARAM,SMOOTH_INPUT,SMOOTH_ATT,c);
            if(polyMask&(1u<<3))raw[3]=params[TUNE_PARAM].getValue()/12.+std::round(fractal::safe(params[OCTAVE_PARAM].getValue()+
                fractal::finite(inputs[OCTAVE_INPUT].getPolyVoltage(c),0)*params[OCTAVE_ATT].getValue(),0,8,3))-4+
                fractal::finite(inputs[PITCH_INPUT].getPolyVoltage(c),0)*params[TUNE_ATT].getValue();
            if(polyMask&(1u<<4))raw[4]=mod(GLIDE_PARAM,GLIDE_INPUT,GLIDE_ATT,c);
            if(polyMask&(1u<<5))raw[5]=mod(LEVEL_PARAM,LEVEL_INPUT,LEVEL_ATT,c);
            if(polyMask&(1u<<6))raw[6]=params[ZOOM_PARAM].getValue()+fractal::finite(inputs[ZOOM_INPUT].getPolyVoltage(c),0)*params[ZOOM_ATT].getValue()*1.2;
            if(polyMask&(1u<<7))raw[7]=mod(POSITION_PARAM,POSITION_INPUT,POSITION_ATT,c);
            if(polyMask&(1u<<8))raw[8]=mod(POSITION_SLEW_PARAM,POSITION_SLEW_INPUT,POSITION_SLEW_ATT,c);
            if(polyMask&(1u<<9))raw[9]=mod(ZOOM_SLEW_PARAM,ZOOM_SLEW_INPUT,ZOOM_SLEW_ATT,c);
        }
        if(!reset&&raw==lastControls[c])return;
        auto& t=targets[c];const auto& before=lastControls[c];
        t.x=fractal::safe(raw[0],0,1,.5);t.y=fractal::safe(raw[1],0,1,.5);
        t.smoothing=fractal::safe(raw[2],0,1,.25);
        if(reset||raw[3]!=before[3]||raw[12]!=before[12])t.frequency=std::min(lastSampleRate*.42,
            261.6255653005986*std::exp2(fractal::safe(raw[3],-16,16,-.25)));
        if(reset||raw[4]!=before[4])t.glideMs=fractal::glideSeconds(raw[4])*1000;
        if(reset||raw[5]!=before[5])t.volume=std::pow(10.,(-60+57*fractal::safe(raw[5],0,1,42./57))/20);
        bool zoom=raw[11]>=.5;t.logZoom=zoom?fractal::safe(raw[6],0,6,0):0;
        t.position=zoom?fractal::safe(raw[7],0,1,.5):.5;
        t.mode=static_cast<fractal::live::PositionMode>(int(std::round(fractal::safe(raw[13],0,2,1))));
        t.positionSlewMs=1000*fractal::safe(raw[8],0,5,.08);
        t.zoomSlewMs=1000*fractal::safe(raw[9],0,1,.035);
        t.autoLevel=raw[10]>=.5?1:0;
        lastControls[c]=raw;
        if(reset)voices[c].initialize(t);else voices[c].setTargets(t);
    }

    void process(const ProcessArgs& args) override {
        if(args.sampleRate!=lastSampleRate) {
            lastSampleRate=args.sampleRate;
            for(auto& v:voices)v.setSampleRate(lastSampleRate);
            telemetryRemaining=1;
        }
        if(importReady.load(std::memory_order_acquire)) {
            for(int i=0;i<16;++i) {
                saveTone(i,incoming.sets[i],incoming.valid[i]);
            }
            if(incoming.hasCurrent)applyTone(incoming.current);
            forcedChannels.store(incoming.channels,std::memory_order_relaxed);
            noticeCode.store(4,std::memory_order_relaxed);
            importReady.store(false,std::memory_order_release);
        }
        unsigned mask=uiRandomize.load(std::memory_order_relaxed);
        if(mask)mask=uiRandomize.exchange(0,std::memory_order_relaxed);
        bool all=triggered(4,NEW_ALL_INPUT,NEW_ALL);
        for(int i=0;i<4;++i)if(triggered(i,NEW_A_INPUT+i,NEW_A+i)||all||(mask&(1u<<i))) {
            uint32_t before=exactSeeds[i].load(std::memory_order_relaxed);
            uint32_t seed=1+randomizer.next()%MAX_EXACT_SEED;
            if(seed==before)seed=seed%MAX_EXACT_SEED+1;
            setSeed(i,seed);flash[i]=.12;
        }
        bool store=triggered(5,STORE_INPUT,STORE_PARAM),recall=triggered(6,RECALL_INPUT,RECALL_PARAM);
        int si=uiStore.load(std::memory_order_relaxed),ri=uiRecall.load(std::memory_order_relaxed);
        if(si>=0)si=uiStore.exchange(-1,std::memory_order_relaxed);
        if(ri>=0)ri=uiRecall.exchange(-1,std::memory_order_relaxed);
        if(store)si=selectedSlot();if(recall)ri=selectedSlot();
        if(si>=0&&si<16) {
            updateSeedParameters();Tone t=currentTone();saveTone(si,t);
            flash[4]=.18;noticeCode.store(1,std::memory_order_relaxed);
        }
        if(ri>=0&&ri<16) {
            Tone t;
            if(recallTone(ri,t)) {
                applyTone(t);flash[5]=.18;noticeCode.store(2,std::memory_order_relaxed);
            }else noticeCode.store(3,std::memory_order_relaxed);
        }
        if((divider++&15u)==0) {
            updateSeedParameters();
            fractal::live::SourceRequest next;next.family=currentSeeds();
            next.roughness=fractal::safe(mod(ROUGH_PARAM,ROUGH_INPUT,ROUGH_ATT,0),0,1,.68);
            if(next!=requested) {requested=next;builder.request.store(next);}
            if(previous<0) {
                int ready=builder.takeReady(requested.family,active>=0?builder.slots[active].version:std::numeric_limits<uint64_t>::max());
                if(ready>=0) {
                    previous=active;active=ready;bankMix=previous<0?1:0;
                    if(previous>=0)builder.slots[previous].state.store(fractal::live::SourceWorker::FADING,std::memory_order_release);
                }
            }
        }
        const int count=channels();outputs[AUDIO_OUTPUT].setChannels(count);
        bool connected=outputs[AUDIO_OUTPUT].isConnected();
        auto* bank=active>=0?&builder.slots[active].bank:nullptr;
        auto* old=previous>=0?&builder.slots[previous].bank:nullptr;
        const int renderCount = connected ? count : (scope.visibleTicks ? 1 : 0);
        if(renderCount && bank) {
            // Polyphonic window controls generally have different trajectories;
            // avoid probing a shared cache when that input topology cannot help.
            uint32_t polyMask=polyControls();
            auto common=controlValues(0);
            auto* windowCache=(polyMask&0x3c0u)?nullptr:&sharedFrame;
            for(int c=0;c<renderCount;++c) {
                updateTarget(c,!outputWasConnected||c>=voiceChannels,&common,polyMask);
                outputs[AUDIO_OUTPUT].setVoltage(static_cast<float>(5*voices[c].process(bank,old,bankMix,windowCache)),c);
            }
            if(scope.armed)
                scope.sample(outputs[AUDIO_OUTPUT].getVoltage(0), voices[0].phase, voices[0].playFrequency(), lastSampleRate);
            if(!connected) outputs[AUDIO_OUTPUT].setVoltage(0);
            processedVoiceSamples+=renderCount;voiceChannels=renderCount;outputWasConnected=true;
        }else {
            // An unpatched, hidden module needs no oscillator or filter work.
            for(int c=0;c<count;++c)outputs[AUDIO_OUTPUT].setVoltage(0,c);
            outputWasConnected=false;voiceChannels=0;
        }
        if(previous>=0) {
            bankMix=std::min(1.,bankMix+1./(lastSampleRate*.015));
            if(bankMix>=1) {
                builder.slots[previous].state.store(fractal::live::SourceWorker::FREE,std::memory_order_release);previous=-1;
            }
        }
        if(--telemetryRemaining==0) {
            scope.tick();
            telemetryRemaining=std::max(1u,static_cast<unsigned>(lastSampleRate/30));
            if(!renderCount||!bank)updateTarget(0,false);
            const auto& c=voices[0].current();
            const auto& display=renderCount&&bank?c:targets[0];
            liveX.store(display.x,std::memory_order_relaxed);liveY.store(display.y,std::memory_order_relaxed);
            liveHz.store(renderCount&&bank?voices[0].playFrequency():targets[0].frequency,std::memory_order_relaxed);liveGlideMs.store(targets[0].glideMs,std::memory_order_relaxed);
            livePosition.store(renderCount&&bank?voices[0].windowPosition():display.position,std::memory_order_relaxed);liveLogZoom.store(display.logZoom,std::memory_order_relaxed);
            liveRough.store(active>=0?builder.slots[active].request.roughness:requested.roughness,std::memory_order_relaxed);
            liveChannels.store(count,std::memory_order_relaxed);
            for(int i=0;i<6;++i) {flash[i]=std::max(0.,flash[i]-telemetryRemaining*args.sampleTime);lights[NEW_A_LIGHT+i].setBrightness(flash[i]>0?1:0);}
            lights[BUSY_LIGHT].setBrightness(builder.busy.load(std::memory_order_relaxed)?1:0);
            lights[ERROR_LIGHT].setBrightness(builder.failed.load(std::memory_order_relaxed)?1:0);
        }
    }
    void onRandomize(const RandomizeEvent& e) override { Module::onRandomize(e);uiRandomize.fetch_or(15); }
    std::string panelReadout(int control) {
        double v=0;
        switch(control) {
            case 0:v=params[X_PARAM].getValue();break;case 1:v=params[Y_PARAM].getValue();break;
            case 2:v=std::exp2(params[ZOOM_PARAM].getValue());break;case 3:v=params[POSITION_PARAM].getValue();break;
            case 4:v=params[ROUGH_PARAM].getValue();break;case 5:v=params[SMOOTH_PARAM].getValue();break;
            case 6:v=liveHz.load(std::memory_order_relaxed);break;case 7:v=params[OCTAVE_PARAM].getValue();break;
            case 8:v=liveGlideMs.load(std::memory_order_relaxed);break;case 9:v=-60+57*params[LEVEL_PARAM].getValue();break;
            case 10:v=1000*params[POSITION_SLEW_PARAM].getValue();break;case 11:v=1000*params[ZOOM_SLEW_PARAM].getValue();break;
        }
        return fractal::panel::readout(control,v);
    }
    std::string bankName(int i) { std::lock_guard<std::mutex> lock(namesMutex);return i>=0&&i<16?names[i]:""; }
    void setBankName(int i,std::string name) {
        if(i<0||i>=16)return;if(name.size()>48)name.resize(48);
        for(char& c:name)if(c=='\n'||c=='\r'||c=='\t')c=' ';
        std::lock_guard<std::mutex> lock(namesMutex);names[i]=name;
    }
    static json_t* seedsToJson(const fractal::SeedSet& s) {
        json_t* j=json_object();json_t* seeds=json_array();json_t* algos=json_array();
        for(int i=0;i<4;++i) {json_array_append_new(seeds,json_integer(s.seeds[i]));json_array_append_new(algos,json_integer(s.algorithms[i]));}
        json_object_set_new(j,"seeds",seeds);json_object_set_new(j,"algorithms",algos);return j;
    }
    static bool seedsFromJson(json_t* j,fractal::SeedSet& s) {
        if(!json_is_object(j))return false;
        auto* seeds=json_object_get(j,"seeds");auto* algos=json_object_get(j,"algorithms");
        if(!json_is_array(seeds)||json_array_size(seeds)!=4||!json_is_array(algos)||json_array_size(algos)!=4)return false;
        for(int i=0;i<4;++i) {
            auto* a=json_array_get(seeds,i);auto* b=json_array_get(algos,i);
            if(!json_is_integer(a)||!json_is_integer(b))return false;
            auto seed=json_integer_value(a),algo=json_integer_value(b);
            if(seed<1||seed>MAX_EXACT_SEED||algo<0||algo>=fractal::ALGORITHMS)return false;
            s.seeds[i]=static_cast<uint32_t>(seed);s.algorithms[i]=static_cast<int>(algo);
        }
        return true;
    }
    static json_t* toneToJson(const Tone& t) {
        json_t* j=seedsToJson(t.seeds);
        if(t.hasControls) {
            json_t* values=json_array();for(float v:t.controls)json_array_append_new(values,json_real(v));
            json_object_set_new(j,"controls",values);
        }
        return j;
    }
    bool toneFromJson(json_t* j,Tone& t) {
        if(!seedsFromJson(j,t.seeds))return false;
        json_t* values=json_object_get(j,"controls");if(!values)return true;
        if(!json_is_array(values))return false;
        const int n=int(json_array_size(values));
        if(n!=26&&n!=27)return false;
        t.controls[26]=fractal::live::Scan; // previous native releases always scanned
        for(int i=0;i<n;++i) {
            auto* n=json_array_get(values,i);if(!json_is_number(n))return false;
            double v=json_number_value(n);auto* q=getParamQuantity(toneIds()[i]);
            if(!std::isfinite(v)||v<q->minValue||v>q->maxValue)return false;
            t.controls[i]=static_cast<float>(v);
        }
        t.hasControls=true;return true;
    }
    json_t* dataToJson() override {
        std::lock_guard<std::mutex> lock(importMutex);
        bool pending=importReady.load(std::memory_order_acquire);
        json_t* root=json_object();json_t* banks=json_array();
        json_object_set_new(root,"format",json_string("jingleengle-fractal-xy-tones"));
        json_object_set_new(root,"version",json_integer(2));
        json_object_set_new(root,"current",toneToJson(pending&&incoming.hasCurrent?incoming.current:currentTone()));
        json_object_set_new(root,"channels",json_integer(pending?incoming.channels:forcedChannels.load()));
        std::lock_guard<std::mutex> nameLock(namesMutex);
        for(int i=0;i<16;++i) {
            Tone t;bool valid;
            if(pending) {valid=incoming.valid[i];t=incoming.sets[i];}
            else {
                valid=false;
                for(int attempt=0;attempt<8&&!valid;++attempt) {
                    valid=recallTone(i,t);
                    if(!memories.occupied[i].load())break;
                }
            }
            json_t* b=json_object();json_object_set_new(b,"name",json_string(names[i].c_str()));
            json_object_set_new(b,"data",valid?toneToJson(t):json_null());json_array_append_new(banks,b);
        }
        json_object_set_new(root,"banks",banks);return root;
    }
    bool browserTone(json_t* root,Tone& tone) {
        auto* settings=json_object_get(root,"settings");if(!json_is_object(settings)||!seedsFromJson(settings,tone.seeds))return false;
        tone=currentTone();if(!seedsFromJson(settings,tone.seeds))return false;
        const char* keys[]={"x","y","roughness","smoothing","tune","octave","glideMs","volumeDb"};
        for(int i=0;i<8;++i) {
            auto* n=json_object_get(settings,keys[i]);if(!n)continue;if(!json_is_number(n))return false;
            double v=json_number_value(n);if(!std::isfinite(v))return false;
            if(i==6)v=fractal::glideNormalized(v/1000.);if(i==7)v=(v+60)/57;
            auto* q=getParamQuantity(toneIds()[i]);tone.controls[i]=fractal::clamp(v,q->minValue,q->maxValue);
        }
        const char* keys2[]={"zoom","position","positionSlewMs","zoomSlewMs"};
        for(int i=0;i<4;++i) {
            auto* n=json_object_get(settings,keys2[i]);if(!n)continue;if(!json_is_number(n))return false;
            double v=json_number_value(n);if(!std::isfinite(v))return false;
            if(i==0)v=std::log2(fractal::clamp(v,1,64));if(i>=2)v/=1000;
            auto* q=getParamQuantity(toneIds()[16+i]);tone.controls[16+i]=fractal::clamp(v,q->minValue,q->maxValue);
        }
        for(int i=0;i<2;++i) {
            auto* b=json_object_get(settings,i?"zoomEnabled":"autoLevel");if(b) {if(!json_is_boolean(b))return false;tone.controls[24+i]=json_is_true(b)?1:0;}
        }
        const auto* mode=json_object_get(settings,"positionMode");
        const std::string motion=json_is_string(mode)?json_string_value(mode):"scan";
        tone.controls[26]=motion=="crossfade"?2:motion=="bend"?1:0;
        auto* dummy=json_object_get(json_object_get(root,"panel"),"dummyCV");
        if(json_is_object(dummy)){
            const char* keys[]={"x","y","roughness","smoothing","tune","octave","glideMs","volumeDb","zoom","position","positionSlewMs","zoomSlewMs"};
            for(int i=0;i<12;++i){
                auto* value=json_object_get(dummy,keys[i]);
                if(json_is_number(value))tone.controls[i<8?8+i:20+i-8]=fractal::safe(json_number_value(value),-1,1);
            }
        }
        return true;
    }
    bool queueImport(json_t* root) {
        if(!json_is_object(root))return false;
        auto* f=json_object_get(root,"format");if(!json_is_string(f))return false;
        std::string format=json_string_value(f);
        Import parsed;std::array<std::string,16> parsedNames;
        if(format=="jingleengle-fractal-zoom-tone"||format=="jingleengle-fractal-tone") {
            if(!browserTone(root,parsed.current))return false;parsed.hasCurrent=true;
            // A single-tone import preserves the 16 existing bank slots.
            for(int i=0;i<16;++i) {
                parsed.valid[i]=memories.occupied[i].load();
                if(parsed.valid[i]&&!recallTone(i,parsed.sets[i]))return false;
                parsedNames[i]=bankName(i);
            }
            parsed.channels=forcedChannels.load();
        }else {
            auto* version=json_object_get(root,"version");if(!json_is_integer(version))return false;
            if(!((format=="jingleengle-fractal-xy-tones"&&json_integer_value(version)==2)||
                 (format=="jingleengle-fractal-xy-seeds"&&json_integer_value(version)==1)))return false;
            auto* banks=json_object_get(root,"banks");if(!json_is_array(banks)||json_array_size(banks)!=16)return false;
            auto* current=json_object_get(root,"current");if(current) {if(!toneFromJson(current,parsed.current))return false;parsed.hasCurrent=true;}
            auto* ch=json_object_get(root,"channels");if(ch) {
                if(!json_is_integer(ch)||json_integer_value(ch)<0||json_integer_value(ch)>16)return false;
                parsed.channels=static_cast<int>(json_integer_value(ch));
            }
            for(int i=0;i<16;++i) {
                auto* b=json_array_get(banks,i);if(!json_is_object(b))return false;
                auto* n=json_object_get(b,"name");if(n&&!json_is_string(n))return false;if(n)parsedNames[i]=json_string_value(n);
                auto* data=json_object_get(b,"data");if(!data)return false;
                if(!json_is_null(data)) {if(!toneFromJson(data,parsed.sets[i]))return false;parsed.valid[i]=true;}
            }
        }
        std::lock_guard<std::mutex> lock(importMutex);if(importReady.load())return false;
        incoming=parsed;for(int i=0;i<16;++i)setBankName(i,parsedNames[i]);
        importReady.store(true,std::memory_order_release);return true;
    }
    void dataFromJson(json_t* root) override { queueImport(root); }
};

inline std::string SeedQuantity::getDisplayValueString() {
    auto* m=dynamic_cast<FractalXY*>(module);int i=paramId-FractalXY::SEED_A;
    if(!m||i<0||i>=4)return string::f("%.0f",getValue());
    uint32_t pending=m->pendingSeeds[i].load(std::memory_order_relaxed);
    return std::to_string(pending?pending:m->exactSeeds[i].load(std::memory_order_relaxed));
}
inline void SeedQuantity::setDisplayValueString(std::string value) {
    auto* m=dynamic_cast<FractalXY*>(module);int i=paramId-FractalXY::SEED_A;
    if(!m||i<0||i>=4)return;
    char* end=nullptr;errno=0;unsigned long long n=std::strtoull(value.c_str(),&end,10);
    while(end&&*end==' ')++end;
    if(errno==0&&end!=value.c_str()&&end&&!*end&&n>=1&&n<=FractalXY::MAX_EXACT_SEED)
        m->pendingSeeds[i].store(static_cast<uint32_t>(n),std::memory_order_relaxed);
}
