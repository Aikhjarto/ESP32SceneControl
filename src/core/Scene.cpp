#include "core/Scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/Motion.h"
#include "core/Sequence.h"

namespace ss {

static constexpr int64_t kMaxRejoinUs = 10'000'000;
// A carried-over speed difference decays at no more than about this acceleration (deg/s^2;
// 0 -> 600 deg/s in 0.3 s), but over at least kMinSpeedDecayS. Peak acceleration of the
// speed term is 3.94 * |e1| / decay time (maximum of the Hermite basis' second derivative).
static constexpr float kSettleAccel = 2000.0f;
static constexpr float kMinSpeedDecayS = 0.15f;

void Scene::tick(int64_t dtUs) {
    const int64_t sceneDtUs = running_ ? dtUs : 0;
    tUs_ += sceneDtUs;
    // A new cycle length (automatic period after editing any servo, or $/scene/period_ms)
    // shifts the timeline of every return-mode servo: they settle onto it smoothly.
    const uint32_t prevPeriod = periodMs_;
    periodMs_ = effectivePeriod();
    const bool periodChanged = prevPeriod != 0 && periodMs_ != prevPeriod;

    for (int i = 0; i < kMaxServos; ++i) {
        const ServoConfig& s = cfg_.servo[i];
        ServoState& st = st_[i];
        if (!live(i)) {
            st.active = false;
            st.vel = 0;  // re-enabling starts from rest
            continue;
        }
        const float tgt = targetDeg(s, tUs_, periodMs_);
        if (!st.valid) {  // first output after boot: position unknown, start on target
            st.out = tgt;
            st.vel = 0;
            st.valid = st.active = true;
            continue;
        }
        if (!st.active) {  // re-enabled
            st.active = true;
            startRejoin(i, false);
        } else if (periodChanged && s.mode == Mode::Return) {
            startRejoin(i, true);
        }

        const float prev = st.out;
        if (st.testing) {
            float diff = st.testDeg - st.out;
            float step = s.maxDps > 0 ? s.maxDps * dtUs / 1e6f : std::fabs(diff);
            st.out += std::clamp(diff, -step, step);
        } else if (st.stopped) {
            // hold
        } else if (st.rejoining) {
            const float tS = (tUs_ - st.t0Us) / 1e6f;
            if (tS >= st.durUs / 1e6f) {
                st.rejoining = false;
                st.out = tgt;
            } else {
                // Never beyond the servo's own range of motion (or where it started from).
                const float off = settleOffset(st.e0, st.e1, st.durUs / 1e6f, st.speedDecayS, tS);
                st.out = std::clamp(tgt + off, st.lo, st.hi);
            }
        } else {
            st.out = tgt;
        }
        st.vel = dtUs > 0 ? (st.out - prev) * 1e6f / dtUs : 0;
    }

    const uint32_t inCycleMs = static_cast<uint32_t>(periodMs_ ? (tUs_ / 1000) % periodMs_ : tUs_ / 1000);
    for (int o = 0; o < kMaxOutputs; ++o) {
        OutputState& os = ost_[o];
        const bool wasActive = os.active;
        os.active = outLive(o);
        if (!os.active) continue;
        if (os.testing) {
            os.on = os.testOn;
        } else if (!os.stopped && (running_ || os.refresh || !wasActive)) {
            // While the scene is held, outputs keep their state; only Release, per-output Go
            // and newly enabled outputs take the (frozen) scene's state once.
            Sequence seq;  // parsed each tick (only wired outputs): never stale after an edit
            std::string err;
            parseSequence(cfg_.out[o].seq, seq, err);
            os.on = sequenceState(seq, inCycleMs);
        }
        os.refresh = false;
    }
    outTimeUs_ = tUs_;
    outDtUs_ = sceneDtUs;
}

int64_t Scene::blendDurationUs(const ServoConfig& s, float distDeg) const {
    // Long enough that the correction itself stays below the servo's speed limit.
    int64_t durUs = static_cast<int64_t>(cfg_.scene.rejoinMs) * 1000;
    if (s.maxDps > 0) durUs = std::max(durUs, static_cast<int64_t>(1.875f * distDeg / s.maxDps * 1e6f));
    return std::min(durUs, kMaxRejoinUs);
}

uint32_t Scene::effectivePeriod() const { return effectivePeriodMs(cfg_, present_, outPresent_); }

// Target speed at scene time t, measured over the same interval as the output speed, so
// that a servo exactly on its target has no speed difference to settle.
float Scene::targetVelocity(const ServoConfig& s, int64_t tUs) const {
    const uint32_t period = periodMs_ ? periodMs_ : effectivePeriod();
    const int64_t h = outDtUs_ > 0 ? outDtUs_ : 1000;
    const int64_t t1 = outDtUs_ > 0 ? tUs : tUs + h / 2;
    return (targetDeg(s, t1, period) - targetDeg(s, t1 - h, period)) * 1e6f / h;
}

// Settle onto the target from wherever the servo is: the output becomes target + offset, and
// the offset (position and speed difference at the start) decays to zero with zero speed and
// acceleration at the end. keepSpeed continues the servo's current motion (edits and
// scene-period changes, also mid-settle); otherwise it starts from rest (Go, Release,
// reset, re-enabling). Used for every transition, so nothing ever jumps.
void Scene::startRejoin(int servo, bool keepSpeed) {
    ServoState& st = st_[servo];
    if (!st.valid || st.stopped || st.testing) return;
    const ServoConfig& s = cfg_.servo[servo];
    const uint32_t period = periodMs_ ? periodMs_ : effectivePeriod();
    // Anchor at the time the current output belongs to.
    st.e0 = st.out - targetDeg(s, outTimeUs_, period);
    st.e1 = (keepSpeed && running_ ? st.vel : 0) - targetVelocity(s, outTimeUs_);
    st.durUs = blendDurationUs(s, std::fabs(st.e0));
    st.speedDecayS = std::min(std::max(kMinSpeedDecayS, 3.94f * std::fabs(st.e1) / kSettleAccel), st.durUs / 1e6f);
    st.lo = std::min({s.startDeg, s.stopDeg, st.out});
    st.hi = std::max({s.startDeg, s.stopDeg, st.out});
    st.t0Us = outTimeUs_;
    st.rejoining = true;
}

void Scene::reset() {
    tUs_ = 0;
    outTimeUs_ = 0;  // outputs now continue from scene time 0
    for (int i = 0; i < kMaxServos; ++i) startRejoin(i, false);
}

void Scene::setServoRun(int servo, bool run) {
    if (servo < 0) {
        for (int i = 0; i < kMaxServos; ++i) setServoRun(i, run);
        return;
    }
    ServoState& st = st_[servo];
    if (st.stopped == !run) return;
    st.stopped = !run;
    if (run) startRejoin(servo, false);
}

void Scene::test(int servo, float deg) {
    ServoState& st = st_[servo];
    st.testing = true;
    st.testDeg = deg;
    st.rejoining = false;
}

void Scene::release(int servo) {
    if (servo < 0) {
        for (int i = 0; i < kMaxServos; ++i) release(i);
        return;
    }
    if (!st_[servo].testing) return;
    st_[servo].testing = false;
    startRejoin(servo, false);
}

void Scene::rejoin(int servo) {
    if (servo < 0) {
        for (int i = 0; i < kMaxServos; ++i) startRejoin(i, true);
        return;
    }
    startRejoin(servo, true);
}

void Scene::setOutputRun(int out, bool run) {
    if (out < 0) {
        for (int o = 0; o < kMaxOutputs; ++o) setOutputRun(o, run);
        return;
    }
    ost_[out].stopped = !run;
    if (run) ost_[out].refresh = true;  // on Go it takes the scene's state at the next tick
}

void Scene::outputTest(int out, bool on) {
    ost_[out].testing = true;
    ost_[out].testOn = on;
}

void Scene::outputRelease(int out) {
    if (out < 0) {
        for (int o = 0; o < kMaxOutputs; ++o) outputRelease(o);
        return;
    }
    ost_[out].testing = false;
    ost_[out].refresh = true;  // back to the sequence at the next tick, also while held
}

std::string Scene::outputs() const {
    std::string out = "[OUT:";
    bool first = true;
    for (int o = 0; o < kMaxOutputs; ++o) {
        if (!ost_[o].active) continue;
        out += (first ? "" : ",") + std::to_string(o) + (ost_[o].on ? "=1" : "=0");
        first = false;
    }
    out += ']';
    return out;
}

static void appendList(std::string& out, const char* label, const std::string& list) {
    if (list.empty()) return;
    out += '|';
    out += label;
    out += ':';
    out += list;
}

std::string Scene::status() const {
    std::string stopped, rejoin, test, outStopped, outTest;
    auto add = [](std::string& l, int i) {
        if (!l.empty()) l += ',';
        l += std::to_string(i);
    };
    for (int i = 0; i < kMaxServos; ++i) {
        if (!live(i)) continue;
        if (st_[i].stopped) add(stopped, i);
        if (st_[i].rejoining) add(rejoin, i);
        if (st_[i].testing) add(test, i);
    }
    for (int o = 0; o < kMaxOutputs; ++o) {
        if (!outLive(o)) continue;
        if (ost_[o].stopped) add(outStopped, o);
        if (ost_[o].testing) add(outTest, o);
    }
    char head[64];
    snprintf(head, sizeof(head), "<%s|T:%.3f|Cyc:%lu", running_ ? "Run" : "Hold", tUs_ / 1e6,
             static_cast<unsigned long>(cycle()));
    std::string out = head;
    appendList(out, "Stop", stopped);
    appendList(out, "Rejoin", rejoin);
    appendList(out, "Test", test);
    appendList(out, "OStop", outStopped);
    appendList(out, "OTest", outTest);
    out += '>';
    return out;
}

std::string Scene::positions() const {
    std::string out = "[POS:";
    bool first = true;
    for (int i = 0; i < kMaxServos; ++i) {
        if (!hasOutput(i)) continue;
        char buf[16];
        snprintf(buf, sizeof(buf), "%s%d=%.1f", first ? "" : ",", i, st_[i].out);
        out += buf;
        first = false;
    }
    out += ']';
    return out;
}

}  // namespace ss
