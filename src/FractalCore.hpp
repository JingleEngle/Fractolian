#pragma once
// Original synthesis code for JingleEngle Fractal XY.
// No Rack dependencies; the same implementation is used by the regression tests.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace fractal {
constexpr double PI = 3.1415926535897932384626433832795;
constexpr double TAU = 2.0 * PI;
constexpr int SIZE = 2048;
constexpr int ROWS = 65;
constexpr int BANDS = 10;
// Rack parameters are floats. Every seed in this range is represented EXACTLY.
constexpr uint32_t MAX_SEED = 16777215;
constexpr std::array<const char*, 10> ALGORITHM_NAMES{{
    "Midpoint", "Smooth fBm", "Ridged fBm", "Harmonic", "Tent", "Takagi", "Fibonacci", "Brownian", "Julia", "Mandelbrot"
}};
constexpr int ALGORITHMS = static_cast<int>(ALGORITHM_NAMES.size());
inline const std::array<int, BANDS>& caps() {
    static const std::array<int, BANDS> c{{512,256,128,64,32,16,8,4,2,1}};
    return c;
}
inline double clamp(double v, double lo, double hi) { return std::max(lo, std::min(hi,v)); }
inline double finite(double v, double fallback) { return std::isfinite(v) ? v : fallback; }
inline double safe(double v, double lo, double hi, double fallback = 0.0) {
    return clamp(finite(v,fallback),lo,hi);
}
inline uint32_t seedValue(double n) {
    return static_cast<uint32_t>(std::llround(safe(n,1,MAX_SEED,197703)));
}
inline const char* algorithmName(int i) {
    return ALGORITHM_NAMES[std::max(0,std::min(ALGORITHMS-1,i))];
}
struct Random {
    uint32_t state;
    explicit Random(uint32_t seed) : state(seed) {}
    uint32_t next() {
        state += 0x6D2B79F5u;
        uint32_t t = (state ^ (state >> 15)) * (1u | state);
        t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
        return t ^ (t >> 14);
    }
    double unit() { return static_cast<double>(next()) / 4294967296.0; }
};
inline std::array<double,4> weights(double x, double y) {
    x = safe(x,0,1,0.5); y = safe(y,0,1,0.5);
    return {{(1-x)*(1-y),x*(1-y),(1-x)*y,x*y}};
}
inline double frequency(double semitones, double octave, double pitchCV = 0) {
    const double oct = safe(octave,0,8,4);
    const double note = safe(semitones,-12,12,9);
    const double v = safe(pitchCV,-20,20);
    return 261.6255653005986 * std::exp2(clamp(oct-4+note/12+v,-16,16));
}
// GLIDE is normalized for a useful knob travel, with an exact zero at its stop.
// Display/typed values are in milliseconds; the DSP uses seconds.
inline double glideSeconds(double normalized) {
    return std::expm1(safe(normalized,0,1) * std::log(5001.0)) / 1000.0;
}
inline double glideNormalized(double seconds) {
    return std::log1p(safe(seconds,0,5)*1000.0)/std::log(5001.0);
}
inline double response(double sampleRate, double seconds) {
    if (!(seconds > 0)) return 1.0;
    return -std::expm1(-1.0/(sampleRate*seconds));
}
struct Glide {
    double value = 440.0;
    double lastTau = -1.0, lastRate = -1.0, coefficient = 1.0;
    double process(double target, double tau, double sampleRate) {
        sampleRate = safe(sampleRate,8000,768000,48000);
        tau = safe(tau,0,5,0.02);
        target = safe(target,0.05,sampleRate*0.42,440);
        if (tau != lastTau || sampleRate != lastRate) {
            coefficient = response(sampleRate,tau); lastTau = tau; lastRate = sampleRate;
        }
        value += (target-value)*coefficient;
        return value;
    }
};
inline void midpointDivide(std::vector<double>& data,int a,int b,double amp,double rough,Random& rng) {
    if (b-a <= 1) return;
    int m = (a+b)/2;
    data[m] = (data[a]+data[b])*0.5+(rng.unit()*2-1)*amp;
    midpointDivide(data,a,m,amp*rough,rough,rng);
    midpointDivide(data,m,b,amp*rough,rough,rng);
}
// All algorithms are finite-resolution periodic fractal constructions.
// Algorithm 0 is a direct port of the original HTML's seeded midpoint method.
inline std::vector<double> rawWave(double rough,uint32_t seed,int algorithm = 0,int size = SIZE) {
    if (size < 4 || size > 16384 || (size & (size-1)))
        throw std::invalid_argument("Wave size must be a power of two, 4..16384");
    rough = safe(rough,0,1,0.5);
    algorithm = std::max(0,std::min(ALGORITHMS-1,algorithm));
    Random rng(seed);
    std::vector<double> data(static_cast<size_t>(size)+1,0.0);
    if (algorithm == 0) {
        data[size/2] = 1;
        midpointDivide(data,0,size/2,rough,rough,rng);
        midpointDivide(data,size/2,size,rough,rough,rng);
    } else if (algorithm <= 2) {
        // A shared fundamental keeps a useful pitched identity at roughness=0.
        for (int i=0;i<size;++i) data[i] = -0.5*std::cos(TAU*i/size);
        double amp = rough*0.6;
        for (int knots=4;knots<=size/2;knots*=2) {
            std::vector<double> v(knots);
            for (auto& value : v) value = rng.unit()*2-1;
            const int width = size/knots;
            for (int offset = 0; offset < width; ++offset) {
                const double f = static_cast<double>(offset)/width;
                const double t = f*f*f*(f*(f*6-15)+10); // shared quintic weights
                for (int k = 0; k < knots; ++k) {
                    double n = v[k]+(v[(k+1)&(knots-1)]-v[k])*t;
                    if (algorithm == 2) n = 1.0-2.0*std::abs(n);
                    data[k*width+offset] += n*amp;
                }
            }
            amp *= rough;
        }
    } else {
        for(int i=0;i<size;++i)data[i]=-.5*std::cos(TAU*i/size);
        if(algorithm<=6){
            const int octaves[]={2,4,8,16,32,64},fibonacci[]={2,3,5,8,13,21,34,55};
            const int* series=algorithm==6?fibonacci:octaves;
            const double aligned=algorithm==5?rng.unit():0;
            double amp=rough*.6;
            for(int h=0;h<(algorithm==6?8:6)&&amp>0;++h){
                const int harmonic=series[h];
                if(harmonic>size/2)break;
                const double shift=algorithm==5?aligned*harmonic:rng.unit();
                for(int i=0;i<size;++i){
                    const double q=shift+double(i)*harmonic/size;
                    data[i]+=amp*((algorithm==4||algorithm==5)?1-4*std::abs(q-std::floor(q)-.5):std::cos(TAU*q));
                }
                amp*=rough;
            }
        } else if(algorithm==7){
            std::vector<double> walk(size+1);
            for(int i=1;i<=size;++i)walk[i]=walk[i-1]+(2*rng.unit()-1);
            const double drift=walk[size]/size,amount=rough*std::sqrt(12./size);
            for(int i=0;i<size;++i)data[i]+=amount*(walk[i]-i*drift);
        } else {
            const double constants[][2]={{-.8,.156},{-.72,.25},{-.13,.75},{.285,.01}};
            const int k=int(rng.unit()*4);const bool julia=algorithm==8;
            const double phase=rng.unit(),radius=julia?.72+.24*rng.unit():.46+.18*rng.unit();
            const double cx=julia?.14*(rng.unit()-.5):-.68+.18*(rng.unit()-.5),cy=.14*(rng.unit()-.5);
            for(int i=0;i<size;++i){
                const double angle=TAU*(double(i)/size+phase);
                const double px=cx+radius*std::cos(angle),py=cy+radius*std::sin(angle);
                const double cr=julia?constants[k][0]:px,ci=julia?constants[k][1]:py;
                double x=julia?px:0,y=julia?py:0,energy=0;
                for(int n=0;n<10;++n){
                    const double next=x*x-y*y+cr;y=2*x*y+ci;x=next;
                    const double rr=x*x+y*y;
                    if(rr>=16){energy+=10-n;break;}
                    energy+=std::min(1.,rr*.25);
                }
                data[i]+=1.2*rough*(energy*.1-.5);
            }
        }
    }
    double mean=0,square=0,peak=0;
    for(int i=0;i<size;++i) mean+=data[i];
    mean/=size;
    for(int i=0;i<size;++i) {
        data[i]-=mean; square+=data[i]*data[i]; peak=std::max(peak,std::abs(data[i]));
    }
    const double scale=std::min(0.38/std::max(1e-9,std::sqrt(square/size)),0.9/std::max(1e-9,peak));
    for(int i=0;i<size;++i) data[i]*=scale;
    data[size]=data[0];
    return data;
}
inline void fft(std::vector<double>& re,std::vector<double>& im,bool inverse=false) {
    const int n=static_cast<int>(re.size());
    if(n<2 || (n&(n-1)) || im.size()!=re.size()) throw std::invalid_argument("Invalid FFT size");
    for(int i=1,j=0;i<n;i++) {
        int bit=n>>1;
        for(;j&bit;bit>>=1) j^=bit;
        j^=bit;
        if(i<j) { std::swap(re[i],re[j]);std::swap(im[i],im[j]); }
    }
    for(int len=2;len<=n;len<<=1) {
        const double angle=(inverse?TAU:-TAU)/len,wr=std::cos(angle),wi=std::sin(angle);
        for(int start=0;start<n;start+=len) {
            double ur=1,ui=0;
            for(int j=0;j<len/2;++j) {
                const int a=start+j,b=a+len/2;
                const double vr=re[b]*ur-im[b]*ui,vi=re[b]*ui+im[b]*ur;
                re[b]=re[a]-vr; im[b]=im[a]-vi; re[a]+=vr; im[a]+=vi;
                const double nr=ur*wr-ui*wi; ui=ur*wi+ui*wr;ur=nr;
            }
        }
    }
    if(inverse) for(int i=0;i<n;++i) { re[i]/=n;im[i]/=n; }
}
struct Bank {
    uint32_t seed=197703;
    int algorithm=0;
    std::vector<float> tables;
};
inline std::shared_ptr<const Bank> buildBank(uint32_t seed,int algorithm=0) {
    std::shared_ptr<Bank> bank=std::make_shared<Bank>();
    bank->seed=seed;bank->algorithm=algorithm;
    const int stride=SIZE+1;
    bank->tables.resize(static_cast<size_t>(BANDS)*ROWS*stride);
    std::vector<double> re(SIZE),im(SIZE),rr(SIZE),ii(SIZE);
    double peak=0;
    for(int row=0;row<ROWS;++row) {
        auto raw=rawWave(static_cast<double>(row)/(ROWS-1),seed,algorithm);
        std::copy_n(raw.begin(),SIZE,re.begin());std::fill(im.begin(),im.end(),0);
        fft(re,im);
        for(int band=0;band<BANDS;++band) {
            std::fill(rr.begin(),rr.end(),0);std::fill(ii.begin(),ii.end(),0);
            int cap=caps()[band];
            for(int k=1;k<=cap;++k) {
                double taper=cap>=8 && k>cap*0.8 ? 0.5+0.5*std::cos(PI*(k-cap*0.8)/(cap*0.2)) : 1;
                rr[k]=re[k]*taper;ii[k]=im[k]*taper;
                rr[SIZE-k]=re[SIZE-k]*taper;ii[SIZE-k]=im[SIZE-k]*taper;
            }
            fft(rr,ii,true);
            const int offset=(band*ROWS+row)*stride;
            for(int i=0;i<SIZE;++i) { bank->tables[offset+i]=static_cast<float>(rr[i]);peak=std::max(peak,std::abs(rr[i])); }
            bank->tables[offset+SIZE]=bank->tables[offset];
        }
    }
    const double scale=0.95/std::max(0.95,peak);
    if(scale<1) for(auto& value:bank->tables) value=static_cast<float>(value*scale);
    return bank;
}
struct Lookup {
    int phaseIndex=0,row=0,band=0;
    double phaseMix=0,roughMix=0,bandMix=1;
};
inline Lookup lookup(double phase,double rough,double frequencyHz,double sampleRate) {
    Lookup l;
    phase=finite(phase,0);phase-=std::floor(phase);
    double p=phase*SIZE; l.phaseIndex=static_cast<int>(p);l.phaseMix=p-l.phaseIndex;
    double r=safe(rough,0,1,0.5)*(ROWS-1);l.row=std::min(ROWS-2,static_cast<int>(r));l.roughMix=r-l.row;
    const double allowed=safe(sampleRate,8000,768000,48000)*0.42/std::max(1.0,finite(frequencyHz,440));
    l.band=static_cast<int>(clamp(std::ceil(std::log2(caps()[0]/allowed)),0,BANDS-1));
    l.bandMix=l.band==BANDS-1 ? 1 : clamp(std::log2(allowed/caps()[l.band]),0,1);
    return l;
}
inline double sampleBand(const Bank& b,const Lookup& l,int band) {
    const int a=(band*ROWS+l.row)*(SIZE+1)+l.phaseIndex,c=a+SIZE+1;
    double v0=b.tables[a]+(b.tables[a+1]-b.tables[a])*l.phaseMix;
    double v1=b.tables[c]+(b.tables[c+1]-b.tables[c])*l.phaseMix;
    return v0+(v1-v0)*l.roughMix;
}
inline double sample(const Bank& b,const Lookup& l) {
    double a=sampleBand(b,l,l.band);
    if(l.bandMix>=1 || l.band>=BANDS-1) return a;
    double v=sampleBand(b,l,l.band+1);
    return v+(a-v)*l.bandMix;
}
struct BankSet {
    std::array<std::shared_ptr<const Bank>,4> corners;
    bool valid() const { for(auto& b:corners) if(!b)return false;return true; }
};
inline double morph(const BankSet& b,const Lookup& l,double x,double y) {
    double a=sample(*b.corners[0],l),bb=sample(*b.corners[1],l);
    double c=sample(*b.corners[2],l),d=sample(*b.corners[3],l);
    double top=a+(bb-a)*x,bottom=c+(d-c)*x;
    return top+(bottom-top)*y;
}
struct Controls {
    double frequency=440,roughness=0.5,smoothing=0.4,x=0.5,y=0.5,level=1,glide=0.02;
};
class Voice {
    double sampleRate_=48000,controlStep_=0,dc_=0;
    double filter1_=0,filter2_=0,lastX_=0,lastY_=0,fadeIn_=0;
    Controls current_;
public:
    double phase=0;
    Glide pitch;
    explicit Voice(double rate=48000) { setSampleRate(rate); }
    void setSampleRate(double rate) {
        sampleRate_=safe(rate,8000,768000,48000);
        controlStep_=response(sampleRate_,0.02);dc_=std::exp(-TAU*3/sampleRate_);
    }
    double sampleRate() const { return sampleRate_; }
    const Controls& current() const { return current_; }
    void initialize(const Controls& c) {
        current_=c;pitch.value=safe(c.frequency,0.05,sampleRate_*0.42,440);
    }
    double process(const BankSet* bank,const BankSet* old,double bankMix,const Controls& t) {
        // Fixed 20 ms SHAPE smoothing; GLIDE only changes the pitch response.
        current_.roughness+=(safe(t.roughness,0,1,0.5)-current_.roughness)*controlStep_;
        current_.smoothing+=(safe(t.smoothing,0,1,0.4)-current_.smoothing)*controlStep_;
        current_.x+=(safe(t.x,0,1,0.5)-current_.x)*controlStep_;
        current_.y+=(safe(t.y,0,1,0.5)-current_.y)*controlStep_;
        current_.level+=(safe(t.level,0,1,1)-current_.level)*controlStep_;
        current_.frequency=pitch.process(t.frequency,t.glide,sampleRate_);
        const Lookup l=lookup(phase,current_.roughness,current_.frequency,sampleRate_);
        double value=0;
        if(bank) {
            value=morph(*bank,l,current_.x,current_.y);
            if(old) {
                double previous=morph(*old,l,current_.x,current_.y);
                value=previous+(value-previous)*clamp(bankMix,0,1);
            }
            fadeIn_+= (1-fadeIn_)*controlStep_;
        }
        const double cutoff=std::min(sampleRate_*0.45,current_.frequency*std::exp2(7*(1-current_.smoothing)));
        const double k=-std::expm1(-TAU*cutoff/sampleRate_);
        filter1_+=(value-filter1_)*k;filter2_+=(filter1_-filter2_)*k;
        value+=(filter2_-value)*std::min(1.0,current_.smoothing*5);
        const double dcRemoved=value-lastX_+dc_*lastY_;lastX_=value;lastY_=dcRemoved;
        phase+=current_.frequency/sampleRate_;phase-=std::floor(phase);
        double output=dcRemoved*current_.level*fadeIn_*5.0;
        if(!std::isfinite(output)) { filter1_=filter2_=lastX_=lastY_=0;return 0; }
        return clamp(output,-9.8,9.8);
    }
};
// Hysteretic rising edge. Holding a gate high generates only ONE seed request.
struct Trigger {
    bool high=false;
    bool process(double volts) {
        volts=finite(volts,0);
        if(high) { if(volts<=0.1) high=false; return false; }
        if(volts>=1) { high=true;return true; }
        return false;
    }
};
} // namespace fractal
