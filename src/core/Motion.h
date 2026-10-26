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

// Offset that decays from (position e0, speed e1) at u = 0 to zero at u = 1 over durS
// seconds, with zero speed and acceleration at the end (quintic Hermite basis).
inline float settleOffset(float e0, float e1, float durS, float u) {
    const float u2 = u * u, u3 = u2 * u;
    const float h0 = 1 + u3 * (-10 + u * (15 - 6 * u));      // 1 -> 0
    const float h1 = u + u3 * (-6 + u * (8 - 3 * u));        // slope 1 -> 0
    return e0 * h0 + e1 * durS * h1;
}

// Time in ms one servo occupies within a scene cycle (delay included); 0 for ping-pong,
// which runs independent of the cycle.
uint32_t servoCycleMs(const ServoConfig& s);

// Scene cycle length: the configured period, or the longest servo cycle when periodMs is 0.
// present (optional, kMaxServos entries) excludes servos without hardware.
uint32_t effectivePeriodMs(const Config& cfg, const bool* present = nullptr);

// Target angle of a servo at scene time tUs, a pure function of the scene clock.
float targetDeg(const ServoConfig& s, int64_t tUs, uint32_t periodMs);

// Peak angular speed of the configured move in deg/s.
float peakDps(const ServoConfig& s);

// Angle (0..rangeDeg) → pulse width in µs, including inversion, trim and clamping.
float pulseUs(const ServoConfig& s, float deg);

}  // namespace ss
