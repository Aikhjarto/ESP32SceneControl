#include "core/Scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/Motion.h"
#include "core/Sequence.h"

namespace ss {

static constexpr int64_t kMaxRejoinUs = 10'000'000;
// A settle that keeps the servo's speed v lets it run on (speedRunOn) for T1 seconds: peak
// run-on 0.1975 * |v| * T1, peak acceleration 3.94 * |v| / T1. T1 is chosen for at most
// kSettleAccel (deg/s^2), at least kMinSpeedDecayS, and shorter when needed to stay inside
// the servo's start..stop range: the run-on never exceeds kRoomFactor * |v| * T1 times the
// share of the room that the blend toward the target has not used yet (max over the settle
// of speedRunOn / (1 - blendCurve)), so blend plus run-on cannot pass the range boundary.
// Close to the boundary that means a hard stop, as the profile itself would make there.
static constexpr float kSettleAccel = 2000.0f;
static constexpr float kMinSpeedDecayS = 0.15f;
static constexpr float kRoomFactor = 0.402f;
static constexpr float kAccelFactor = 3.94f;

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
                // Blend from where the servo was to its moving target (stays between the two,
                // so inside the range), plus the run-on of the speed it had.
                st.out = st.from + (tgt - st.from) * blendCurve(tS * 1e6f / st.durUs) +
                         speedRunOn(st.e1, st.speedDecayS, tS);
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
            os.on = sequenceState(seq_[o], inCycleMs, periodMs_);
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

uint32_t Scene::effectivePeriod() const {
    refreshSequences();
    return effectivePeriodMs(cfg_, present_, outPresent_, outLastMs_);
}

// Sequences are parsed once per configuration change, not every tick.
void Scene::refreshSequences() const {
    if (!seqDirty_) return;
    for (int o = 0; o < kMaxOutputs; ++o) {
        std::string err;
        if (!parseSequence(cfg_.out[o].seq, seq_[o], err)) seq_[o] = Sequence();
        outLastMs_[o] = seq_[o].count ? static_cast<int32_t>(seq_[o].timeMs[seq_[o].count - 1]) : -1;
    }
    seqDirty_ = false;
}

// Target speed at scene time t, measured over the same interval as the output speed, so
// that a servo exactly on its target has no speed difference to settle.
float Scene::targetVelocity(const ServoConfig& s, int64_t tUs) const {
    const uint32_t period = periodMs_ ? periodMs_ : effectivePeriod();
    const int64_t h = outDtUs_ > 0 ? outDtUs_ : 1000;
    const int64_t t1 = outDtUs_ > 0 ? tUs : tUs + h / 2;
    return (targetDeg(s, t1, period) - targetDeg(s, t1 - h, period)) * 1e6f / h;
}

// Settle onto the target from wherever the servo is: a minimum-jerk blend from the current
// position to the moving target, which starts from rest and ends exactly on the target with
// its speed. keepSpeed (edits, scene-period changes, also mid-settle) adds the run-on of the
// servo's current speed, so a moving servo neither jumps nor stalls; without it (Go,
// Release, reset, re-enabling) the servo starts from rest. A servo already on its new
// trajectory at the right speed needs no settle at all. Used for every transition.
void Scene::startRejoin(int servo, bool keepSpeed) {
    ServoState& st = st_[servo];
    if (!st.valid || st.stopped || st.testing) return;
    const ServoConfig& s = cfg_.servo[servo];
    const uint32_t period = periodMs_ ? periodMs_ : effectivePeriod();
    // Anchor at the time the current output belongs to.
    const float tgt0 = targetDeg(s, outTimeUs_, period);
    const float vel = keepSpeed && running_ ? st.vel : 0;
    if (keepSpeed && std::fabs(st.out - tgt0) < 0.05f && std::fabs(vel - targetVelocity(s, outTimeUs_)) < 2.0f) {
        st.rejoining = false;  // e.g. an edit that does not change where the servo is going now
        return;
    }
    st.from = st.out;
    st.e1 = vel;
    // Speed part: gentle enough for kSettleAccel, short enough not to run out of the range.
    // A servo that an edit left outside its new range does not run on further out.
    float t1 = std::max(kMinSpeedDecayS, kAccelFactor * std::fabs(st.e1) / kSettleAccel);
    if (st.e1 != 0) {
        const float lo = std::min(s.startDeg, s.stopDeg), hi = std::max(s.startDeg, s.stopDeg);
        const float room = std::max(0.0f, st.e1 > 0 ? hi - st.out : st.out - lo);
        t1 = std::min(t1, room / (kRoomFactor * std::fabs(st.e1)));
    }
    st.speedDecayS = t1;
    // The whole settle lasts at least as long as its speed part.
    st.durUs = std::max(blendDurationUs(s, std::fabs(st.out - tgt0)), static_cast<int64_t>(t1 * 1e6f));
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
