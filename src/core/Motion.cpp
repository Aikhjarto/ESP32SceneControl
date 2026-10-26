#include "core/Motion.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "core/Sequence.h"

namespace ss {

static constexpr float kPi = 3.14159265358979f;

static float trapAccelShare(Profile p, float accelPct) {
    if (p == Profile::Tri) return 0.5f;
    return std::clamp(accelPct, 0.0f, 50.0f) / 100.0f;
}

float profilePos(Profile p, float u, float accelPct) {
    if (u <= 0) return 0;
    if (u >= 1) return 1;
    switch (p) {
        case Profile::Const:
            return u;
        case Profile::Trap:
        case Profile::Tri: {
            float a = trapAccelShare(p, accelPct);
            if (a <= 0) return u;
            float vp = 1.0f / (1.0f - a);
            if (u < a) return vp * u * u / (2 * a);
            if (u <= 1 - a) return vp * (u - a / 2);
            float r = 1 - u;
            return 1 - vp * r * r / (2 * a);
        }
        case Profile::Sine:
            return (1 - std::cos(kPi * u)) / 2;
        case Profile::Bell:
        default:
            return blendCurve(u);
    }
}

float profilePeakFactor(Profile p, float accelPct) {
    switch (p) {
        case Profile::Const:
            return 1;
        case Profile::Trap:
        case Profile::Tri:
            return 1.0f / (1.0f - trapAccelShare(p, accelPct));
        case Profile::Sine:
            return kPi / 2;
        case Profile::Bell:
        default:
            return 1.875f;
    }
}

uint32_t servoCycleMs(const ServoConfig& s) {
    switch (s.mode) {
        case Mode::Once:
            return s.delayMs + s.durationMs;
        case Mode::Return:
            return s.delayMs + 2 * s.durationMs + s.dwellMs;
        default:
            return 0;
    }
}

uint32_t effectivePeriodMs(const Config& cfg, const bool* present, const bool* outPresent) {
    if (cfg.scene.periodMs > 0) return cfg.scene.periodMs;
    uint32_t longest = 0;
    for (int i = 0; i < kMaxServos; ++i) {
        const ServoConfig& s = cfg.servo[i];
        if (servoActive(s) && (!present || present[i])) longest = std::max(longest, servoCycleMs(s));
    }
    // An output sequence needs its last switch point inside the cycle, plus time for the
    // last state to show: rounded up to the next full second.
    for (int o = 0; o < kMaxOutputs; ++o) {
        if (!outputActive(cfg.out[o]) || (outPresent && !outPresent[o])) continue;
        Sequence seq;
        std::string err;
        if (parseSequence(cfg.out[o].seq, seq, err) && seq.count > 0) {
            longest = std::max(longest, (seq.timeMs[seq.count - 1] / 1000 + 1) * 1000);
        }
    }
    return std::max<uint32_t>(longest, 100);
}

// Position along start→stop for local time t (ms) into a move of duration d.
static float along(const ServoConfig& s, float t, float d) {
    float u = d > 0 ? t / d : 1;
    return s.startDeg + (s.stopDeg - s.startDeg) * profilePos(s.profile, u, s.accelPct);
}

static float back(const ServoConfig& s, float t, float d) {
    float u = d > 0 ? t / d : 1;
    return s.stopDeg + (s.startDeg - s.stopDeg) * profilePos(s.profile, u, s.accelPct);
}

float targetDeg(const ServoConfig& s, int64_t tUs, uint32_t periodMs) {
    const float d = static_cast<float>(s.durationMs);
    const float dwell = static_cast<float>(s.dwellMs);
    const int64_t delayUs = static_cast<int64_t>(s.delayMs) * 1000;

    if (s.mode == Mode::Once) {
        // Absolute scene time: moves on the first cycle only, then holds the stop angle.
        float t = static_cast<float>(tUs - delayUs) / 1000.0f;
        if (t <= 0) return s.startDeg;
        return t >= d ? s.stopDeg : along(s, t, d);
    }

    if (s.mode == Mode::PingPong) {
        int64_t localUs = tUs - delayUs;
        if (localUs <= 0) return s.startDeg;
        int64_t cycleUs = static_cast<int64_t>(2 * s.durationMs + 2 * s.dwellMs) * 1000;
        if (cycleUs <= 0) return s.startDeg;
        float t = static_cast<float>(localUs % cycleUs) / 1000.0f;
        if (t < d) return along(s, t, d);
        t -= d;
        if (t < dwell) return s.stopDeg;
        t -= dwell;
        if (t < d) return back(s, t, d);
        return s.startDeg;
    }

    // Mode::Return: start→stop, dwell, stop→start, repeated every scene cycle.
    int64_t periodUs = static_cast<int64_t>(periodMs) * 1000;
    int64_t inCycleUs = periodUs > 0 ? tUs % periodUs : tUs;
    float t = static_cast<float>(inCycleUs - delayUs) / 1000.0f;
    if (t <= 0) return s.startDeg;
    if (t < d) return along(s, t, d);
    t -= d;
    if (t < dwell) return s.stopDeg;
    t -= dwell;
    if (t < d) return back(s, t, d);
    return s.startDeg;
}

float peakDps(const ServoConfig& s) {
    if (s.durationMs == 0) return INFINITY;
    float dist = std::fabs(s.stopDeg - s.startDeg);
    return profilePeakFactor(s.profile, s.accelPct) * dist / (s.durationMs / 1000.0f);
}

float pulseUs(const ServoConfig& s, float deg) {
    float range = s.rangeDeg > 0 ? s.rangeDeg : 180;
    deg = std::clamp(deg, 0.0f, range);
    if (s.invert) deg = range - deg;
    float us = s.minUs + (s.maxUs - s.minUs) * deg / range + s.trimUs;
    return std::clamp(us, 100.0f, 3000.0f);
}

}  // namespace ss
