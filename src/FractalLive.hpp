#pragma once
// Continuous Fractal XY scanner, ported from the optimized browser LiveSynth.
// Source preparation is worker-only. Voice owns no bank and does no allocation,
// locking, reference-count operation or reclamation in process().
#include "FractalCore.hpp"
#if !defined(FRACTAL_LIVE_DISABLE_SSE2) && (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#include <emmintrin.h>
#define FRACTAL_LIVE_HAS_SSE2 1
#endif

namespace fractal { namespace live {

constexpr int OVERSAMPLE = 2;
constexpr int TAPS = 47;
constexpr int GAIN_COLUMNS = 512;
constexpr int GAIN_ROWS = 25;
constexpr double LN2 = 0.693147180559945309417232121458;

enum PositionMode { Scan, Bend, Crossfade };
struct Settings {
    PositionMode mode=Scan;
    double frequency = 220;
    double x = .5, y = .5, smoothing = .25;
    double volume = 0.12589254117941673; // pow(10, -18 / 20)
    double position = .5, logZoom = 0, autoLevel = 1;
    double glideMs = 20, positionSlewMs = 80, zoomSlewMs = 35;
};

struct Table {
    int size=SIZE,stride=1;
    std::vector<double> coeff;
    // Energy and first-moment integrals are needed only during construction.
    // Keeping just area in the playable bank saves two thirds of that storage.
    std::vector<double> area;
};

struct Corner {
    uint32_t seed = 197703;
    int algorithm = 0;
    double roughness = .68;
    std::array<float, SIZE + 1> source{};
    std::array<Table, BANDS> live{};
    std::array<float, GAIN_COLUMNS * GAIN_ROWS> gainProfile{};
};

struct Bank {
    std::array<std::shared_ptr<const Corner>, 4> corners;
    // Publishers reusing fixed Bank slots increment this on every publication.
    uint64_t revision = 0;
    bool valid() const {
        for (const auto& corner : corners) if (!corner) return false;
        return true;
    }
};

namespace detail {

struct FullIntegrals {
    std::vector<double> energy;
    std::vector<double> moment;
};

inline void prepareTable(const float* samples, Table& table, FullIntegrals* full,int n=SIZE) {
    table.size=n;table.stride=n/SIZE;table.coeff.resize(n*4);table.area.resize(n+1);
    if(full){full->energy.resize(n+1);full->moment.resize(n+1);}
    const double h = 1.0 / n;
    double area = 0, energy = 0, moment = 0;
    for (int i = 0; i < n; ++i) {
        const double ym = samples[(i + n - 1) % n], y = samples[i];
        const double yp = samples[(i + 1) % n], ypp = samples[(i + 2) % n];
        const double d = y, c = (yp - ym) * .5;
        const double b = ym - 2.5 * y + 2 * yp - .5 * ypp;
        const double a = .5 * (ypp - ym) + 1.5 * (y - yp);
        const int at = i * 4;
        table.coeff[at] = d; table.coeff[at + 1] = c;
        table.coeff[at + 2] = b; table.coeff[at + 3] = a;
        const double I = d + c / 2 + b / 3 + a / 4;
        area += h * I;
        table.area[i + 1] = area;
        if (full) {
            const double E = d*d + d*c + (c*c + 2*d*b)/3
                + (2*d*a + 2*c*b)/4 + (b*b + 2*c*a)/5
                + 2*b*a/6 + a*a/7;
            const double J = d/2 + c/3 + b/4 + a/5;
            energy += h * E;
            moment += h*h*(i*I + J);
            full->energy[i + 1] = energy;
            full->moment[i + 1] = moment;
        }
    }
}

struct Coordinate {
    int index = 0;
    double fraction = 0, turns = 0;
};

inline Coordinate coordinate(double phase) {
    Coordinate out;
    out.turns = std::floor(phase);
    const double q = (phase - out.turns) * SIZE;
    out.index = std::min(SIZE - 1, static_cast<int>(q));
    out.fraction = q - out.index;
    return out;
}

inline Coordinate tableCoordinate(const Table& table,Coordinate point) {
    if(table.stride!=1){
        const double part=point.fraction*table.stride;
        const int whole=int(part);
        point.index=point.index*table.stride+whole;
        point.fraction=part-whole;
    }
    return point;
}
inline std::array<double, 4> primitive(const Table& table,
                                     const FullIntegrals& full, double phase) {
    const Coordinate point = tableCoordinate(table,coordinate(phase));
    const int i = point.index, k = i * 4;
    const double u = point.fraction, turns = point.turns;
    const double d = table.coeff[k], c = table.coeff[k + 1];
    const double b = table.coeff[k + 2], a = table.coeff[k + 3], u2 = u*u;
    const double value = ((a*u + b)*u + c)*u + d;
    const double I = u*(d + u*(c/2 + u*(b/3 + u*a/4)));
    const double E = u*(d*d + u*(d*c + u*((c*c + 2*d*b)/3
        + u*((2*d*a + 2*c*b)/4 + u*((b*b + 2*c*a)/5
        + u*(a*b/3 + u*a*a/7))))));
    const double J = u2*(d/2 + u*(c/3 + u*(b/4 + u*a/5)));
    const double area = table.area[i] + I/table.size;
    const double energy = full.energy[i] + E/table.size;
    const double moment = full.moment[i] + (i*I + J)/(table.size*table.size);
    return {{value, turns*table.area[table.size] + area,
        turns*full.energy[table.size] + energy,
        turns*full.moment[table.size] + turns*(turns - 1)*.5*table.area[table.size]
            + turns*area + moment}};
}

inline double variance(const Table& table, const FullIntegrals& full,
                       double start, double span) {
    const auto left = primitive(table, full, start);
    const auto right = primitive(table, full, start + span);
    const double delta = right[0] - left[0], I = right[1] - left[1];
    const double A = I/span;
    const double T = (right[3] - left[3] - start*I)/(span*span);
    const double mean = A - delta*.5;
    return std::max(0.0, (right[2] - left[2])/span - 2*delta*T
                           + delta*delta/3 - mean*mean);
}

inline double gain(double variance, double logZoom, double autoLevel) {
    const double t = clamp(logZoom, 0, 1), mix = t*t*(3 - 2*t)*autoLevel;
    const double g = std::min(256.0, .38/std::sqrt(std::max(0.0, variance) + 1e-8));
    return 1 + (g - 1)*mix;
}

inline void prepareGainProfile(Corner& corner, const FullIntegrals& full) {
    std::array<double, GAIN_COLUMNS> values{}, temp{};
    for (int row = 0; row < GAIN_ROWS; ++row) {
        const double z = static_cast<double>(row)*6/(GAIN_ROWS - 1);
        const double span = std::exp2(-z);
        for (int i = 0; i < GAIN_COLUMNS; ++i)
            values[i] = variance(corner.live[0], full,
                static_cast<double>(i)/GAIN_COLUMNS - span*.5, span);
        const int radius = std::max(1, static_cast<int>(std::ceil(span*GAIN_COLUMNS*.25)));
        for (int pass = 0; pass < 2; ++pass) {
            double sum = 0;
            for (int k = -radius; k <= radius; ++k)
                sum += values[(k + GAIN_COLUMNS) % GAIN_COLUMNS];
            for (int i = 0; i < GAIN_COLUMNS; ++i) {
                temp[i] = sum/(2*radius + 1);
                sum += values[(i + radius + 1) % GAIN_COLUMNS]
                     - values[(i - radius + GAIN_COLUMNS) % GAIN_COLUMNS];
            }
            values = temp;
        }
        for (int i = 0; i < GAIN_COLUMNS; ++i)
            corner.gainProfile[row*GAIN_COLUMNS + i]
                = static_cast<float>(gain(values[i], z, 1));
    }
}

struct ProfileCoordinate {
    int a = 0, b = 0, i = 0, j = 0;
    double u = 0, f = 0, autoLevel = 0;
    bool active = false;
};

inline ProfileCoordinate profileCoordinate(double position, double logZoom,
                                           double autoLevel) {
    ProfileCoordinate c;
    if (autoLevel == 0 || logZoom <= 0) return c;
    c.active = true; c.autoLevel = autoLevel;
    const double q = (position - std::floor(position))*GAIN_COLUMNS;
    c.i = static_cast<int>(q); c.u = q - c.i;
    c.j = (c.i + 1) % GAIN_COLUMNS;
    const double r = clamp(logZoom, 0, 6)*(GAIN_ROWS - 1)/6;
    const int row = std::min(GAIN_ROWS - 2, static_cast<int>(r));
    c.f = r - row; c.a = row*GAIN_COLUMNS; c.b = c.a + GAIN_COLUMNS;
    return c;
}

inline double profileGain(const Corner& corner, const ProfileCoordinate& c) {
    if (!c.active) return 1;
    const auto& p = corner.gainProfile;
    // Promote samples BEFORE subtraction: JS Float32Array reads are doubles.
    const double a0 = p[c.a + c.i], a1 = p[c.a + c.j];
    const double b0 = p[c.b + c.i], b1 = p[c.b + c.j];
    const double g0 = a0 + (a1 - a0)*c.u;
    const double g1 = b0 + (b1 - b0)*c.u;
    return 1 + (g0 + (g1 - g0)*c.f - 1)*c.autoLevel;
}

inline double tableValue(const Table& table, const Coordinate& base) {
    const auto p=tableCoordinate(table,base);
    const int k = p.index*4; const double u = p.fraction;
    const auto& c = table.coeff;
    return ((c[k + 3]*u + c[k + 2])*u + c[k + 1])*u + c[k];
}

inline void bounds(const Table& table, const Coordinate& baseLeft,
                   const Coordinate& baseRight, double span,
                   double& delta, double& mean) {
    const auto left=tableCoordinate(table,baseLeft),right=tableCoordinate(table,baseRight);
    const auto& c = table.coeff;
    const int k = left.index*4, l = right.index*4;
    const double u = left.fraction, w = right.fraction;
    const double d = c[k], v = c[k + 1], b = c[k + 2], a = c[k + 3];
    const double first = ((a*u + b)*u + v)*u + d;
    const double area0 = left.turns*table.area[table.size] + table.area[left.index]
        + u*(d + u*(v*.5 + u*(b/3 + u*a*.25)))/table.size;
    const double dd = c[l], vv = c[l + 1], bb = c[l + 2], aa = c[l + 3];
    const double last = ((aa*w + bb)*w + vv)*w + dd;
    const double area1 = right.turns*table.area[table.size] + table.area[right.index]
        + w*(dd + w*(vv*.5 + w*(bb/3 + w*aa*.25)))/table.size;
    delta = last - first;
    mean = (area1 - area0)/span - delta*.5;
}

#if defined(FRACTAL_LIVE_HAS_SSE2)
// Two corners share endpoint coordinates and a span. SSE2 evaluates each
// corner in double precision and reuses the window's reciprocal span.
inline void boundsPair(const Table& table0, const Table& table1,
                       const Coordinate& left, const Coordinate& right,
                       double inverseSpan, double* deltas, double* means) {
    const __m128d half = _mm_set1_pd(.5), quarter = _mm_set1_pd(.25);
    const __m128d third = _mm_set1_pd(1./3), invSize = _mm_set1_pd(1./table0.size);
    const auto endpoint = [&](const Coordinate& base, __m128d& value, __m128d& area) {
        const auto point=tableCoordinate(table0,base);
        const int k = point.index*4;
        const __m128d u = _mm_set1_pd(point.fraction);
        const __m128d d = _mm_set_pd(table1.coeff[k], table0.coeff[k]);
        const __m128d c = _mm_set_pd(table1.coeff[k + 1], table0.coeff[k + 1]);
        const __m128d b = _mm_set_pd(table1.coeff[k + 2], table0.coeff[k + 2]);
        const __m128d a = _mm_set_pd(table1.coeff[k + 3], table0.coeff[k + 3]);
        value = _mm_add_pd(_mm_mul_pd(a, u), b);
        value = _mm_add_pd(_mm_mul_pd(value, u), c);
        value = _mm_add_pd(_mm_mul_pd(value, u), d);
        __m128d I = _mm_add_pd(_mm_mul_pd(b, third), _mm_mul_pd(_mm_mul_pd(u, a), quarter));
        I = _mm_add_pd(_mm_mul_pd(c, half), _mm_mul_pd(u, I));
        I = _mm_add_pd(d, _mm_mul_pd(u, I));
        I = _mm_mul_pd(_mm_mul_pd(u, I), invSize);
        area = _mm_add_pd(_mm_mul_pd(_mm_set1_pd(point.turns),
            _mm_set_pd(table1.area[table0.size], table0.area[table0.size])),
            _mm_set_pd(table1.area[point.index], table0.area[point.index]));
        area = _mm_add_pd(area, I);
    };
    __m128d first, last, area0, area1;
    endpoint(left, first, area0); endpoint(right, last, area1);
    const __m128d delta = _mm_sub_pd(last, first);
    const __m128d mean = _mm_sub_pd(_mm_mul_pd(_mm_sub_pd(area1, area0), _mm_set1_pd(inverseSpan)),
                                     _mm_mul_pd(delta, half));
    _mm_storeu_pd(deltas, delta); _mm_storeu_pd(means, mean);
}
#endif

inline const std::array<double, TAPS>& firCoefficients() {
    static const std::array<double, TAPS> fir = [] {
        std::array<double, TAPS> taps{};
        const double mid = (TAPS - 1)/2.0, cut = .225;
        double sum = 0;
        for (int i = 0; i < TAPS; ++i) {
            const double x = i - mid;
            const double s = x == 0 ? 2*cut : std::sin(TAU*cut*x)/(PI*x);
            const double window = .42 - .5*std::cos(TAU*i/(TAPS - 1))
                + .08*std::cos(2*TAU*i/(TAPS - 1));
            taps[i] = s*window; sum += taps[i];
        }
        for (auto& value : taps) value /= sum;
        return taps;
    }();
    return fir;
}

} // namespace detail

// Worker-only entry point. No shared ownership is touched by Voice::process.
inline std::shared_ptr<const Corner> buildCorner(uint32_t seed, int algorithm,
                                                double roughness) {
    auto result = std::make_shared<Corner>();
    result->seed = seed;
    result->algorithm = std::max(0, std::min(ALGORITHMS - 1, algorithm));
    result->roughness = safe(roughness, 0, 1, .68);
    const auto raw = rawWave(result->roughness, seed, result->algorithm);
    for (int i = 0; i <= SIZE; ++i) result->source[i] = static_cast<float>(raw[i]);

    // The browser rounds the source to Float32 BEFORE its forward FFT.
    std::vector<double> re(SIZE), im(SIZE, 0), rr(SIZE), ii(SIZE);
    for (int i = 0; i < SIZE; ++i) re[i] = result->source[i];
    fft(re, im);
    std::array<std::vector<float>,BANDS> tables;
    double peak=0;
    for(int band=0;band<BANDS;++band){
        const int cap=caps()[band],n=std::max(SIZE,16*cap),stride=n/SIZE;
        rr.assign(n,0);ii.assign(n,0);
        for(int k=1;k<=cap;++k){
            const double taper=stride*(cap>=8&&k>cap*.8?.5+.5*std::cos(PI*(k-cap*.8)/(cap*.2)):1);
            rr[k]=re[k]*taper;ii[k]=im[k]*taper;
            rr[n-k]=re[SIZE-k]*taper;ii[n-k]=im[SIZE-k]*taper;
        }
        fft(rr,ii,true);
        auto& wave=tables[band];wave.assign(rr.begin(),rr.end());wave.push_back(wave.front());
        for(int i=0;i<n;i+=stride)peak=std::max(peak,std::abs(rr[i]));
    }
    const double scale=.95/std::max(.95,peak);
    detail::FullIntegrals full;
    for(int band=0;band<BANDS;++band){
        auto& wave=tables[band];
        if(scale<1)for(float& value:wave)value=float(value*scale);
        detail::prepareTable(wave.data(),result->live[band],band==0?&full:nullptr,int(wave.size())-1);
    }
    detail::prepareGainProfile(*result, full);
    return result;
}

inline double profileGain(const Corner& corner, double position, double logZoom,
                          double autoLevel) {
    return detail::profileGain(corner,
        detail::profileCoordinate(position, logZoom, autoLevel));
}

namespace detail {
    struct WindowCache {
        const Bank* bank = nullptr;
        uint64_t revision = 0;
        double start = std::numeric_limits<double>::quiet_NaN();
        double span = 0, logZoom = 0, autoLevel = 0;
        int band = -1;
        bool next = false;
        std::array<double, 8> delta{}, mean{};
        std::array<double, 4> gain{};
    };
}

// Optional module-local sharing. These are computed window measurements and
// gain TARGETS, never oscillator/filter/slew/gain state. Direct slots avoid a
// search in fully independent modulation. Entries remain valid across samples
// when every input matches, so no per-sample clearing is needed.
struct SharedFrame {
    struct Entry {
        detail::WindowCache window;
        std::array<const Corner*, 4> corners{{nullptr,nullptr,nullptr,nullptr}};
    };
    std::array<Entry, OVERSAMPLE*BANDS*2> entries{};
    uint64_t hits = 0, misses = 0;

    void clear() {
        for (auto& entry : entries) entry.window.bank = nullptr;
        hits = misses = 0;
    }
};

// Both gestures and the audio-rate follower compare the same 24 matched phases.
// The bank is immutable and all probe storage is on the stack.
inline double shapeRate(const Bank& bank,const Settings& c,double from,double distance,
                        double span,int band,double blend){
    const double width=std::min(std::abs(distance),span/128);
    if(width<1e-14)return std::numeric_limits<double>::infinity();
    const double to=from+std::copysign(width,distance);
    const auto w=weights(c.x,c.y);
    double waves[2][24]={};
    for(int side=0;side<2;++side){
        const double pos=side?to:from,start=pos-span*.5;
        const auto left=detail::coordinate(start),right=detail::coordinate(start+span);
        const auto profile=detail::profileCoordinate(pos,c.logZoom,c.autoLevel);
        for(int j=0;j<4;++j){
            const auto& corner=*bank.corners[j];
            const double g=w[j]*detail::profileGain(corner,profile);
            if(g==0)continue;
            for(int offset=0;offset<2;++offset){
                const double weight=g*(offset?1-blend:blend);
                if(!weight||band+offset>=BANDS)continue;
                const auto& table=corner.live[band+offset];
                double delta,mean;
                detail::bounds(table,left,right,span,delta,mean);
                for(int i=0;i<24;++i){
                    const double phase=(i+(i%2?.73:.31))/24.;
                    waves[side][i]+=weight*(detail::tableValue(table,detail::coordinate(start+span*phase))-delta*phase-mean);
                }
            }
        }
    }
    double diff=0,energy=0;
    for(int i=0;i<24;++i){
        const double a=waves[0][i],b=waves[1][i];
        diff+=(b-a)*(b-a);energy+=(a*a+b*b)*.5;
    }
    const double relative=std::sqrt(diff/std::max(energy,24*.025*.025));
    return relative>1e-8?.65*width/relative:std::numeric_limits<double>::infinity();
}

inline void selectBand(double speed,double sr,int& band,double& blend){
    const double level=clamp(std::log2(caps()[0]*speed/(sr*.40)),0,BANDS-1);
    band=int(std::ceil(level));
    blend=band==BANDS-1?1:(level==0?clamp(std::log2(sr*.40/(speed*caps()[0])),0,1):band-level);
}

class Voice {
    using WindowCache = detail::WindowCache;
    struct WindowGeometry {
        double start = std::numeric_limits<double>::quiet_NaN(), span = 0;
        double position = 0, logZoom = 0, autoLevel = 0;
        detail::Coordinate left, right;
        detail::ProfileCoordinate profile;
    };

    double sr_ = 48000, osr_ = 96000;
    Settings current_, target_;
    const Bank* bank_ = nullptr;
    const Bank* old_ = nullptr;
    uint64_t bankRevision_ = 0, oldRevision_ = 0;
    std::array<double, 4> gains_{{1,1,1,1}}, oldGains_{{1,1,1,1}};
    WindowCache cache_, oldCache_;
    WindowGeometry geometry_;
    double gate_ = 0, dc_ = 0, filter1_ = 0, filter2_ = 0, lastX_ = 0, lastY_ = 0;
    double shapeStep_ = 0, gateStep_ = 0, gainStep_ = 0, gainAttack_ = 0, fadeStep_ = 0;
    double motionRise_ = 0, motionFall_ = 0, glideStep_ = 0, positionStep_ = 0, zoomStep_ = 0;
    double glideMs_ = -1, positionSlewMs_ = -1, zoomSlewMs_ = -1;
    double motionSpeed_ = 0, span_ = 1, lastLogZoom_ = std::numeric_limits<double>::quiet_NaN();
    double lastSpeed_ = std::numeric_limits<double>::quiet_NaN();
    double lastFilterFrequency_ = std::numeric_limits<double>::quiet_NaN();
    double lastSmoothing_ = std::numeric_limits<double>::quiet_NaN(), filterK_ = 1, filterMix_ = 1;
    double blend_ = 1;
    int band_ = 0, head_ = 0;
    std::array<double, TAPS*2> history_{};
    uint64_t boundsUpdates_ = 0, frames_ = 0;

    double guide_=.5,follow_=.5,bendStep_=0,pitchStep_=0,bendRatio_=0,playHz_=220;
    double gestureSpeed_=0,gestureScale_=1,gestureLimit_=1e30,shapeLimit_=1e30;
    uint64_t gestureFrame_=0;
    bool hasGesture_=false,largeMove_=false;
    int probeWait_=0,probeDirection_=0;
    double fadeTo_=-1,positionMix_=0;
    int fadeIndex_=0,fadeLength_=1;
    WindowCache incomingCache_,incomingOldCache_;
    std::array<double,4> incomingGains_{},incomingOldGains_{};

    void trackGesture(double position){
        const double gap=hasGesture_?1000.*(frames_-gestureFrame_)/sr_:1e6;
        const double dt=gap>180?40:std::max(1.,gap);
        const double speed=1000*std::abs(position-target_.position)/dt;
        gestureSpeed_=gap>180?speed:gestureSpeed_+(speed-gestureSpeed_)*(-std::expm1(-dt/24));
        hasGesture_=true;gestureFrame_=frames_;
        const double excess=std::max(0.,gestureSpeed_-.6);
        gestureScale_=std::max(.3,1/(1+.65*excess));
        gestureLimit_=(.20+.70/(1+.8*excess))/osr_;
        const double large=clamp(1.5*span_,.04,.25),remaining=std::abs(position-current_.position);
        if(remaining>=large)largeMove_=true;
        else if(remaining<large*.125)largeMove_=false;
    }

    double advanceBend(const Bank& bank){
        const double pos=target_.position,distance=pos-guide_,before=current_.position;
        if(pos==before&&pos==guide_&&pos==follow_)return 0;
        double scale=1;
        if(largeMove_){
            const double near=clamp(1.5*span_,.04,.25)*.25;
            const double u=clamp((std::abs(distance)-near)/(3*near),0,1);
            scale-=.6*u*u*(3-2*u);
        }
        double limit=std::min(.72*current_.frequency*span_*scale/osr_,gestureLimit_);
        const double wanted=distance*bendStep_*scale,coarse=.85*gestureScale_;
        const double floor=(largeMove_?coarse:std::max(.008,std::min(coarse,2.5*std::abs(distance))))/osr_;
        if(std::min(std::abs(wanted),limit)>floor&&std::isfinite(lastSpeed_)){
            const int direction=distance<0?-1:1;
            if(probeWait_--<=0||probeDirection_!=direction){
                shapeLimit_=shapeRate(bank,current_,guide_,distance,span_,band_,blend_)/osr_;
                probeWait_=std::max(1,int(std::round(osr_*.012)))-1;probeDirection_=direction;
            }
            limit=std::min(limit,std::max(floor,shapeLimit_*gestureScale_));
        }else probeWait_=0;
        guide_+=clamp(wanted,-limit,limit);
        follow_+=(guide_-follow_)*bendStep_;
        current_.position+=(follow_-current_.position)*bendStep_;
        if(std::abs(pos-guide_)<1e-12&&std::abs(pos-follow_)<1e-12&&std::abs(pos-current_.position)<1e-12){
            guide_=follow_=current_.position=pos;largeMove_=false;
        }
        return current_.position-before;
    }

    void initializeGains(const Bank* bank, std::array<double, 4>& gains) {
        const auto p = detail::profileCoordinate(current_.position, current_.logZoom,
                                                 current_.autoLevel);
        for (int j = 0; j < 4; ++j) gains[j] = detail::profileGain(*bank->corners[j], p);
    }

    void synchronizeBanks(const Bank* bank, const Bank* old) {
        if (bank != bank_ || bank->revision != bankRevision_) {
            if (old) {
                if (old == bank_ && old->revision == bankRevision_) {
                    oldGains_=gains_;incomingOldGains_=incomingGains_;
                }
                else if (old != old_ || old->revision != oldRevision_) initializeGains(old, oldGains_);
            }
            initializeGains(bank, gains_);
            if(fadeTo_>=0)for(int j=0;j<4;++j)incomingGains_[j]=profileGain(*bank->corners[j],fadeTo_,current_.logZoom,current_.autoLevel);
            cache_.bank = nullptr;
            bank_ = bank;
            bankRevision_ = bank->revision;
        } else if (old && (old != old_ || old->revision != oldRevision_)) {
            initializeGains(old, oldGains_);
        }
        if (old != old_ || (old && old->revision != oldRevision_)) {
            oldCache_.bank = nullptr; old_ = old; oldRevision_ = old ? old->revision : 0;
        }
    }

    void prepareGeometry(double start, double span) {
        if (geometry_.start != start || geometry_.span != span) {
            geometry_.start = start; geometry_.span = span;
            geometry_.left = detail::coordinate(start);
            geometry_.right = detail::coordinate(start + span);
        }
        const double center=start+span*.5;
        if (geometry_.position != center || geometry_.logZoom != current_.logZoom
            || geometry_.autoLevel != current_.autoLevel) {
            geometry_.position = center;
            geometry_.logZoom = current_.logZoom; geometry_.autoLevel = current_.autoLevel;
            geometry_.profile = detail::profileCoordinate(center,
                current_.logZoom, current_.autoLevel);
        }
    }

    bool windowMatches(const WindowCache& cache, const Bank* bank, double start,
                       double span, int band, bool next) const {
        return cache.bank == bank && cache.revision == bank->revision
            && cache.start == start && cache.span == span
            && cache.band == band && cache.next == next
            && cache.logZoom == current_.logZoom && cache.autoLevel == current_.autoLevel;
    }

    void fillWindow(const Bank* bank, double start, double span,
                    int band, bool next, WindowCache& cache) {
        prepareGeometry(start, span);
        cache.bank = bank; cache.start = start; cache.span = span;
        cache.revision = bank->revision;
        cache.band = band; cache.next = next;
        cache.logZoom = current_.logZoom; cache.autoLevel = current_.autoLevel;
        ++boundsUpdates_;
#if defined(FRACTAL_LIVE_HAS_SSE2)
        const double inverseSpan = 1/span;
        for (int j = 0; j < 4; j += 2) {
            const Corner& corner0 = *bank->corners[j];
            const Corner& corner1 = *bank->corners[j + 1];
            detail::boundsPair(corner0.live[band], corner1.live[band],
                geometry_.left, geometry_.right, inverseSpan, cache.delta.data() + j, cache.mean.data() + j);
            if (next) detail::boundsPair(corner0.live[band + 1], corner1.live[band + 1],
                geometry_.left, geometry_.right, inverseSpan, cache.delta.data() + j + 4, cache.mean.data() + j + 4);
            cache.gain[j] = detail::profileGain(corner0, geometry_.profile);
            cache.gain[j + 1] = detail::profileGain(corner1, geometry_.profile);
        }
#else
        for (int j = 0; j < 4; ++j) {
            const Corner& corner = *bank->corners[j];
            detail::bounds(corner.live[band], geometry_.left, geometry_.right, span,
                           cache.delta[j], cache.mean[j]);
            if (next) detail::bounds(corner.live[band + 1], geometry_.left, geometry_.right,
                                    span, cache.delta[j + 4], cache.mean[j + 4]);
            cache.gain[j] = detail::profileGain(corner, geometry_.profile);
        }
#endif
    }

    const WindowCache& prepareWindow(const Bank* bank, double start, double span,
                       int band, bool next, WindowCache& local,
                       SharedFrame* shared, int sub, bool old) {
        if (windowMatches(local, bank, start, span, band, next)) return local;
        if (shared) {
            auto& entry = shared->entries[(sub*BANDS + band)*2 + (old ? 1 : 0)];
            if (windowMatches(entry.window, bank, start, span, band, next)
                && entry.corners[0] == bank->corners[0].get()
                && entry.corners[1] == bank->corners[1].get()
                && entry.corners[2] == bank->corners[2].get()
                && entry.corners[3] == bank->corners[3].get()) {
                ++shared->hits;
                return entry.window;
            }
            ++shared->misses;
            fillWindow(bank, start, span, band, next, entry.window);
            for (int j = 0; j < 4; ++j) entry.corners[j] = bank->corners[j].get();
            return entry.window;
        }
        fillWindow(bank, start, span, band, next, local);
        return local;
    }

    double scan(const Bank* bank, std::array<double, 4>& gains,
                double start, double span, int band, double blend,
                const detail::Coordinate& coordinate, WindowCache& local,
                SharedFrame* shared, int sub, bool old) {
        const bool next = blend < 1 && band < BANDS - 1;
        const auto& cache = prepareWindow(bank, start, span, band, next, local, shared, sub, old);
        double top = 0, bottom = 0;
        for (int j = 0; j < 4; ++j) {
            const double desired = cache.gain[j];
            gains[j] += (desired - gains[j])*(desired < gains[j] ? gainAttack_ : gainStep_);
            const auto& tables = bank->corners[j]->live;
            double value = detail::tableValue(tables[band], coordinate)
                - cache.delta[j]*phase - cache.mean[j];
            if (next) {
                const double b = detail::tableValue(tables[band + 1], coordinate)
                    - cache.delta[j + 4]*phase - cache.mean[j + 4];
                value = b + (value - b)*blend;
            }
            value *= gains[j];
            if (j == 0) top = value*(1 - current_.x);
            else if (j == 1) top += value*current_.x;
            else if (j == 2) bottom = value*(1 - current_.x);
            else bottom += value*current_.x;
        }
        return top*(1 - current_.y) + bottom*current_.y;
    }

public:
    double phase = 0;

    explicit Voice(double rate = 48000) {
        // Force static filter construction outside the audio callback.
        (void)detail::firCoefficients();
        setSampleRate(rate);
        initialize(Settings{});
    }

    void setSampleRate(double rate) {
        sr_ = safe(rate, 8000, 384000, 48000); osr_ = sr_*OVERSAMPLE;
        current_.frequency = std::min(current_.frequency, sr_*.42);
        fadeStep_ = 1/(osr_*.015);
        dc_ = std::exp(-TAU*3/osr_);
        shapeStep_ = response(osr_, .020); gateStep_ = response(osr_, .007);
        gainStep_ = response(osr_, .025); gainAttack_ = response(osr_, .010);
        motionRise_ = response(osr_, .001); motionFall_ = response(osr_, .030);
        glideMs_ = positionSlewMs_ = zoomSlewMs_ = -1;
        lastSpeed_ = lastFilterFrequency_ = lastSmoothing_ = std::numeric_limits<double>::quiet_NaN();
        setTargets(target_);
    }

    void setTargets(const Settings& input) {
        target_.frequency = safe(input.frequency, .05, sr_*.42, 220);
        target_.x = safe(input.x, 0, 1, .5); target_.y = safe(input.y, 0, 1, .5);
        target_.smoothing = safe(input.smoothing, 0, 1, .25);
        target_.volume = safe(input.volume, 0, 1, .12589254117941673);
        const double destination=safe(input.position,0,1,.5);
        if(input.mode!=target_.mode){
            guide_=follow_=current_.position;hasGesture_=largeMove_=false;gestureSpeed_=0;
            gestureScale_=1;gestureLimit_=1e30;probeWait_=0;
        }
        if(input.mode==Bend&&destination!=target_.position)trackGesture(destination);
        target_.mode=input.mode;target_.position=destination;
        target_.logZoom = safe(input.logZoom, 0, 6, 0);
        target_.autoLevel = safe(input.autoLevel, 0, 1, 1);
        target_.glideMs = safe(input.glideMs, 0, 5000, 20);
        target_.positionSlewMs = safe(input.positionSlewMs, 0, 5000, 80);
        target_.zoomSlewMs = safe(input.zoomSlewMs, 0, 1000, 35);
        if (target_.glideMs != glideMs_) {
            glideMs_ = target_.glideMs; glideStep_ = response(osr_, glideMs_/1000);
        }
        if (target_.positionSlewMs != positionSlewMs_) {
            positionSlewMs_ = target_.positionSlewMs;
            positionStep_ = response(osr_, std::max(2.0, positionSlewMs_)/1000);
            const double sec=(positionSlewMs_<32?16+.5*positionSlewMs_:positionSlewMs_)/4000.;
            bendStep_=response(osr_,sec);pitchStep_=response(osr_,std::max(.01,sec*.5));
        }
        if (target_.zoomSlewMs != zoomSlewMs_) {
            zoomSlewMs_ = target_.zoomSlewMs;
            zoomStep_ = response(osr_, std::max(2.0, zoomSlewMs_)/1000);
        }
    }

    void initialize(const Settings& settings) {
        setTargets(settings); current_ = target_;
        guide_=follow_=current_.position;bendRatio_=0;playHz_=current_.frequency;
        gestureSpeed_=0;gestureScale_=1;gestureLimit_=shapeLimit_=1e30;
        probeWait_=0;hasGesture_=largeMove_=false;fadeTo_=-1;positionMix_=0;
        phase = gate_ = filter1_ = filter2_ = lastX_ = lastY_ = motionSpeed_ = 0;
        bank_ = old_ = nullptr;
        bankRevision_ = oldRevision_ = 0;
        gains_.fill(1); oldGains_.fill(1); history_.fill(0);
        cache_ = WindowCache{}; oldCache_ = WindowCache{}; geometry_ = WindowGeometry{};
        lastLogZoom_ = lastSpeed_ = lastFilterFrequency_ = lastSmoothing_
            = std::numeric_limits<double>::quiet_NaN();
        span_ = 1; band_ = 0; blend_ = 1; filterK_ = filterMix_ = 1; head_ = 0;
        boundsUpdates_ = frames_ = 0;
    }

    // bank/old lifetime is controlled by the module. fade is the linear source
    // transition value at the beginning of this HOST sample (15ms at 2x rate).
    double process(const Bank* bank, const Bank* old = nullptr, double fade = 1,
                   SharedFrame* shared = nullptr) {
        if (!bank) return 0;
        synchronizeBanks(bank, old);
        double subFade = clamp(fade, 0, 1);
        for (int sub = 0; sub < OVERSAMPLE; ++sub) {
            auto& c = current_; const auto& t = target_;
            c.frequency += (t.frequency - c.frequency)*glideStep_;
            c.x += (t.x - c.x)*shapeStep_; c.y += (t.y - c.y)*shapeStep_;
            c.smoothing += (t.smoothing - c.smoothing)*shapeStep_;
            c.volume += (t.volume - c.volume)*shapeStep_;
            c.autoLevel += (t.autoLevel - c.autoLevel)*shapeStep_;
            double dp=0;
            const double dz = (t.logZoom - c.logZoom)*zoomStep_;
            c.logZoom += dz; gate_ += (1 - gate_)*gateStep_;

            if (std::abs(t.logZoom - c.logZoom) < 1e-13) c.logZoom = t.logZoom;
            if (std::abs(t.frequency - c.frequency) < 1e-10) c.frequency = t.frequency;
            if (std::abs(t.smoothing - c.smoothing) < 1e-13) c.smoothing = t.smoothing;
            if (std::abs(t.autoLevel - c.autoLevel) < 1e-13) c.autoLevel = t.autoLevel;
            if (c.logZoom != lastLogZoom_) { lastLogZoom_ = c.logZoom; span_ = std::exp2(-c.logZoom); }
            double pitchTarget=0;
            if(fadeTo_<0){
                if(t.mode==Bend){
                    dp=advanceBend(*bank);
                    const double travel=std::abs(dp)*osr_/(span_*c.frequency);
                    pitchTarget=std::max(-.3325800729149828,-.9*std::max(0.,travel-.015));
                }else if(t.mode==Scan){
                    dp=(t.position-c.position)*positionStep_;c.position+=dp;
                    if(std::abs(t.position-c.position)<1e-13)c.position=t.position;
                }else if(!old&&t.position!=c.position){
                    fadeTo_=t.position;fadeIndex_=0;
                    fadeLength_=std::max(1,int(std::ceil(osr_*std::max(12.,t.positionSlewMs)/1000)));
                    for(int j=0;j<4;++j)incomingGains_[j]=profileGain(*bank->corners[j],fadeTo_,c.logZoom,c.autoLevel);
                }
            }
            bendRatio_+=(pitchTarget-bendRatio_)*pitchStep_;
            if(pitchTarget==0&&std::abs(bendRatio_)<1e-11)bendRatio_=0;
            playHz_=c.frequency*(1+bendRatio_);
            const double span = span_, start = c.position - span*.5;
            const double motion = std::abs(dp)*osr_ + span*.5*std::abs(dz)*LN2*osr_;
            motionSpeed_ += (motion - motionSpeed_)*(motion > motionSpeed_ ? motionRise_ : motionFall_);
            if (motionSpeed_ < 1e-12) motionSpeed_ = 0;
            const double speed = std::max(1.0, playHz_*span + motionSpeed_);
            if (speed != lastSpeed_) {
                lastSpeed_ = speed;
                selectBand(speed,sr_,band_,blend_);
            }
            const auto coordinate = detail::coordinate(start + span*phase);
            double value = scan(bank, gains_, start, span, band_, blend_, coordinate, cache_, shared, sub, false);
            if (old && subFade < 1) {
                const double previous = scan(old, oldGains_, start, span, band_, blend_, coordinate, oldCache_, shared, sub, true);
                value = previous + (value - previous)*subFade;
                subFade = std::min(1.0, subFade + fadeStep_);
            }
            if(fadeTo_>=0){
                const double begin=fadeTo_-span*.5;
                const auto point=detail::coordinate(begin+phase*span);
                double incoming=scan(bank,incomingGains_,begin,span,band_,blend_,point,incomingCache_,nullptr,sub,false);
                if(old&&fade<1){
                    const double prior=scan(old,incomingOldGains_,begin,span,band_,blend_,point,incomingOldCache_,nullptr,sub,true);
                    incoming=prior+(incoming-prior)*clamp(fade+sub*fadeStep_,0,1);
                }
                const double u=double(++fadeIndex_)/fadeLength_;
                positionMix_=u*u*u*(10+u*(-15+6*u));value+=(incoming-value)*positionMix_;
                if(fadeIndex_>=fadeLength_){
                    guide_=follow_=c.position=fadeTo_;fadeTo_=-1;positionMix_=0;
                    gains_=incomingGains_;oldGains_=incomingOldGains_;
                }
            }
            if (c.frequency != lastFilterFrequency_ || c.smoothing != lastSmoothing_) {
                lastFilterFrequency_ = c.frequency; lastSmoothing_ = c.smoothing;
                const double cutoff = std::min(sr_*.45, c.frequency*std::exp2(7*(1 - c.smoothing)));
                filterK_ = 1 - std::exp(-TAU*cutoff/osr_);
                filterMix_ = std::min(1.0, c.smoothing*5);
            }
            filter1_ += (value - filter1_)*filterK_;
            filter2_ += (filter1_ - filter2_)*filterK_;
            value += (filter2_ - value)*filterMix_;
            const double dc = value - lastX_ + dc_*lastY_; lastX_ = value; lastY_ = dc;
            value = dc*c.volume*gate_;
            if (!std::isfinite(value)) value = 0;
            if (std::abs(value) > .8)
                value = std::copysign(.8 + .18*std::tanh((std::abs(value) - .8)/.18), value);
            history_[head_] = history_[head_ + TAPS] = value;
            if (++head_ == TAPS) head_ = 0;
            phase += playHz_/osr_; if (phase >= 1) phase -= 1;
        }
        const auto& fir = detail::firCoefficients();
        double output = 0;
        constexpr int middle = (TAPS - 1)/2;
        for (int j = 0; j < middle; ++j)
            output += (history_[head_ + j] + history_[head_ + TAPS - 1 - j])*fir[j];
        output += history_[head_ + middle]*fir[middle];
        ++frames_;
        return std::isfinite(output) ? clamp(output, -.98, .98) : 0;
    }

    const Settings& current() const { return current_; }
    const Settings& target() const { return target_; }
    double sampleRate() const { return sr_; }
    double motionSpeed() const { return motionSpeed_; }
    int band() const { return band_; }
    double blend() const { return blend_; }
    double gate() const { return gate_; }
    const std::array<double, 4>& gains() const { return gains_; }
    const std::array<double, 4>& oldGains() const { return oldGains_; }
    uint64_t boundsUpdates() const { return boundsUpdates_; }
    uint64_t frames() const { return frames_; }
    double playFrequency() const {return playHz_;}
    double windowPosition() const {return fadeTo_<0?current_.position:current_.position+(fadeTo_-current_.position)*positionMix_;}
    double windowLogZoom() const { return current_.logZoom; }
    double windowZoom() const { return std::exp2(current_.logZoom); }
};

}} // namespace fractal::live
