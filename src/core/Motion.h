// Motion math: velocity profiles, per-servo target position over scene time,
// and angle → pulse conversion. Pure functions, unit-tested natively.
#pragma once

#include <cstdint>

#include "core/Config.h"

namespace ss {

// Normalized position s(u) for u in [0,1], with s(0)=0 and s(1)=1.
// accelPct is only used by Profile::Trap.
float profilePos(Profile p, float u, float accelPct);

// Peak of ds/du, i.e. peak velocity as a multiple of (distance / duration).
float profilePeakFactor(Profile p, float accelPct);

// Minimum-jerk curve used for rejoin blends (zero velocity and acceleration at both ends).
inline float blendCurve(float u) {
    if (u <= 0) return 0;
    if (u >= 1) return 1;
    return u * u * u * (10 + u * (-15 + 6 * u));
}

// Offset that decays from position e0 and speed e1 to zero, tS seconds after the start: the
// position part over t0S, the speed part over its own (shorter) t1S, both with zero speed
// and acceleration at their end (quintic Hermite bases). Keeping t1S short bounds how far
// a carried-over speed can push the servo before it turns around.
inline float settleOffset(float e0, float e1, float t0S, float t1S, float tS) {
    float off = 0;
    if (t0S > 0 && tS < t0S) {
        const float u = tS / t0S, u3 = u * u * u;
        off += e0 * (1 + u3 * (-10 + u * (15 - 6 * u)));  // 1 -> 0
    }
    if (t1S > 0 && tS < t1S) {
        const float u = tS / t1S, u3 = u * u * u;
        off += e1 * t1S * (u + u3 * (-6 + u * (8 - 3 * u)));  // slope 1 -> 0
    }
    return off;
}

// Time in ms one servo occupies within a scene cycle (delay included); 0 for ping-pong,
// which runs independent of the cycle.
uint32_t servoCycleMs(const ServoConfig& s);

// Scene cycle length: the configured period or, when periodMs is 0, the longest servo cycle
// and the last switch point of every digital output rounded up to the next full second.
// present / outPresent (optional, kMaxServos / kMaxOutputs entries) exclude items without
// hardware.
uint32_t effectivePeriodMs(const Config& cfg, const bool* present = nullptr, const bool* outPresent = nullptr);

// Target angle of a servo at scene time tUs, a pure function of the scene clock.
float targetDeg(const ServoConfig& s, int64_t tUs, uint32_t periodMs);

// Peak angular speed of the configured move in deg/s.
float peakDps(const ServoConfig& s);

// Angle (0..rangeDeg) → pulse width in µs, including inversion, trim and clamping.
float pulseUs(const ServoConfig& s, float deg);

}  // namespace ss
