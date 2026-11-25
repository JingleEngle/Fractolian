#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

// The engine owns the frame until publication. The UI releases it after copying.
struct OutputScope {
    enum { POINTS = 192 };
    struct Frame {
        std::array<float, POINTS> volts{};
        int count = 0;
    } frame;
    std::atomic<bool> drawn{false}, ready{false};
    int visibleTicks = 0;
    bool armed = false;
    int elapsed = 0, duration = 0, written = 0, wait = 0;
    double interval = 1, nextPoint = 0, lastPhase = -1;

    void tick() {
        if (drawn.exchange(false, std::memory_order_relaxed)) visibleTicks = 8;
        else if (visibleTicks) --visibleTicks;
        if (!visibleTicks) armed = false;
        else if (!armed && !ready.load(std::memory_order_acquire)) {
            armed = true;
            elapsed = written = wait = duration = 0;
            lastPhase = -1;
        }
    }
    void sample(float volts, double phase, double frequency, double rate) {
        if (!duration) {
            bool wrap = phase < lastPhase;
            lastPhase = phase;
            if (!wrap && ++wait < rate * .04) return;
            duration = std::max(4, int(std::round(std::min(rate * .08, 2 * rate / std::max(.05, frequency)))));
            frame.count = std::min<int>(POINTS, duration);
            interval = double(duration - 1) / (frame.count - 1);
            nextPoint = 0;
        }
        if (elapsed++ >= int(nextPoint + .5)) {
            frame.volts[written++] = volts;
            nextPoint += interval;
            if (written == frame.count) {
                armed = false;
                ready.store(true, std::memory_order_release);
            }
        }
    }
    bool read(Frame& dest) {
        if (!ready.load(std::memory_order_acquire)) return false;
        dest = frame;
        ready.store(false, std::memory_order_release);
        return true;
    }
};
