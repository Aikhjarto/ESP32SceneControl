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

// How far a servo that was moving at speed v (deg/s) runs on, tS seconds into a settle whose
// speed part lasts t1S: starts with slope v, ends with zero speed and acceleration (quintic
// Hermite basis). Peaks at 0.1975 * v * t1S; peak acceleration 3.94 * |v| / t1S.
inline float speedRunOn(float v, float t1S, float tS) {
    if (t1S <= 0 || tS >= t1S) return 0;
    const float u = tS / t1S, u3 = u * u * u;
    return v * t1S * (u + u3 * (-6 + u * (8 - 3 * u)));
}

// Time in ms one servo occupies within a scene cycle (delay included); 0 for ping-pong,
// which runs independent of the cycle.
uint32_t servoCycleMs(const ServoConfig& s);

// Scene cycle length: the configured period or, when periodMs is 0, the longest servo cycle
// and the last switch point of every digital output rounded up to the next full second.
// present / outPresent (optional, kMaxServos / kMaxOutputs entries) exclude items without
// hardware. outLastMs (optional, kMaxOutputs entries, -1 = no points) gives each output's
// last switch time when the caller already has the sequences parsed.
uint32_t effectivePeriodMs(const Config& cfg, const bool* present = nullptr, const bool* outPresent = nullptr,
                           const int32_t* outLastMs = nullptr);

// Target angle of a servo at scene time tUs, a pure function of the scene clock.
float targetDeg(const ServoConfig& s, int64_t tUs, uint32_t periodMs);

// Peak angular speed of the configured move in deg/s.
float peakDps(const ServoConfig& s);

// Angle (0..rangeDeg) → pulse width in µs, including inversion, trim and clamping.
float pulseUs(const ServoConfig& s, float deg);

// Pulse of a powered servo. A calibration edit (min_us, max_us, trim_us, range_deg, invert)
// would move it in one step - invert by up to the whole travel - so the pulse glides from
// the old calibration to the new one at the servo's max_dps instead (at once if 0).
class PulseGlide {
public:
    float pulse(const ServoConfig& s, float deg, int64_t dtUs);
    void reset() { valid_ = false; }  // servo off: the next pulse starts without a glide

private:
    bool valid_ = false;
    float offsetUs_ = 0;  // old calibration's pulse minus the new one, decays to 0
    ServoConfig cal_{};   // calibration the offset refers to
};

}  // namespace ss
