#include "core/Scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/Motion.h"

namespace ss {

static constexpr int64_t kMaxRejoinUs = 10'000'000;

void Scene::tick(int64_t dtUs) {
    if (running_) tUs_ += dtUs;
    // A new cycle length (automatic period after editing any servo, or $/scene/period_ms)
    // shifts the timeline of every return-mode servo: they switch over smoothly.
    const uint32_t prevPeriod = periodMs_;
    periodMs_ = effectivePeriodMs(cfg_, present_);
    const bool periodChanged = prevPeriod != 0 && periodMs_ != prevPeriod;

    for (int i = 0; i < kMaxServos; ++i) {
        const ServoConfig& s = cfg_.servo[i];
        ServoState& st = st_[i];
        if (!live(i)) {
            st.active = false;
            continue;
        }
        const float tgt = targetDeg(s, tUs_, periodMs_);
        if (!st.valid) {  // first output after boot: position unknown, start on target
            st.out = tgt;
            st.valid = st.active = true;
            continue;
        }
        if (!st.active) {  // re-enabled
            st.active = true;
            startRejoin(i);
        } else if (periodChanged && s.mode == Mode::Return) {
            startPeriodSwitch(i, prevPeriod);
        }

        if (st.testing) {
            float diff = st.testDeg - st.out;
            float step = s.maxDps > 0 ? s.maxDps * dtUs / 1e6f : std::fabs(diff);
            st.out += std::clamp(diff, -step, step);
        } else if (st.stopped) {
            // hold
        } else if (st.rejoining) {
            float u = st.durUs > 0 ? static_cast<float>(tUs_ - st.t0Us) / st.durUs : 1;
            if (u >= 1) {
                st.rejoining = false;
                st.out = tgt;
            } else {
                st.out = st.from + (tgt - st.from) * blendCurve(u);
            }
        } else if (st.switching) {
            // Blend between the old and the new trajectory: velocity stays continuous, and
            // where both agree (e.g. in the first cycle) the output is exactly unchanged.
            float u = st.durUs > 0 ? static_cast<float>(tUs_ - st.t0Us) / st.durUs : 1;
            if (u >= 1) {
                st.switching = false;
                st.out = tgt;
            } else {
                const float old = targetDeg(s, tUs_, st.oldPeriodMs);
                st.out = old + (tgt - old) * blendCurve(u);
            }
        } else {
            st.out = tgt;
        }
    }
}

int64_t Scene::blendDurationUs(const ServoConfig& s, float distDeg) const {
    // Long enough that the blend's own correction stays below the servo's speed limit.
    int64_t durUs = static_cast<int64_t>(cfg_.scene.rejoinMs) * 1000;
    if (s.maxDps > 0) durUs = std::max(durUs, static_cast<int64_t>(1.875f * distDeg / s.maxDps * 1e6f));
    return std::min(durUs, kMaxRejoinUs);
}

void Scene::startPeriodSwitch(int servo, uint32_t oldPeriodMs) {
    ServoState& st = st_[servo];
    if (!st.valid || st.stopped || st.testing) return;  // they rejoin when released anyway
    if (st.rejoining || st.switching) {                   // already blending: settle from here
        startRejoin(servo);
        return;
    }
    const ServoConfig& s = cfg_.servo[servo];
    float dist = std::fabs(targetDeg(s, tUs_, oldPeriodMs) - targetDeg(s, tUs_, periodMs_));
    st.oldPeriodMs = oldPeriodMs;
    st.durUs = blendDurationUs(s, dist);
    st.t0Us = tUs_;
    st.switching = true;
}

void Scene::startRejoin(int servo) {
    ServoState& st = st_[servo];
    if (!st.valid || st.stopped || st.testing) return;
    const ServoConfig& s = cfg_.servo[servo];
    float dist = std::fabs(targetDeg(s, tUs_, periodMs_ ? periodMs_ : effectivePeriodMs(cfg_, present_)) - st.out);
    st.durUs = blendDurationUs(s, dist);
    st.from = st.out;
    st.t0Us = tUs_;
    st.rejoining = true;
    st.switching = false;
}

void Scene::reset() {
    tUs_ = 0;
    for (int i = 0; i < kMaxServos; ++i) startRejoin(i);
}

void Scene::setServoRun(int servo, bool run) {
    if (servo < 0) {
        for (int i = 0; i < kMaxServos; ++i) setServoRun(i, run);
        return;
    }
    ServoState& st = st_[servo];
    if (st.stopped == !run) return;
    st.stopped = !run;
    if (run) startRejoin(servo);
}

void Scene::test(int servo, float deg) {
    ServoState& st = st_[servo];
    st.testing = true;
    st.testDeg = deg;
    st.rejoining = false;
    st.switching = false;
}

void Scene::release(int servo) {
    if (servo < 0) {
        for (int i = 0; i < kMaxServos; ++i) release(i);
        return;
    }
    if (!st_[servo].testing) return;
    st_[servo].testing = false;
    startRejoin(servo);
}

void Scene::rejoin(int servo) {
    if (servo < 0) {
        for (int i = 0; i < kMaxServos; ++i) startRejoin(i);
        return;
    }
    startRejoin(servo);
}

static void appendList(std::string& out, const char* label, const std::string& list) {
    if (list.empty()) return;
    out += '|';
    out += label;
    out += ':';
    out += list;
}

std::string Scene::status() const {
    std::string stopped, rejoin, test;
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
    char head[64];
    snprintf(head, sizeof(head), "<%s|T:%.3f|Cyc:%lu", running_ ? "Run" : "Hold", tUs_ / 1e6,
             static_cast<unsigned long>(cycle()));
    std::string out = head;
    appendList(out, "Stop", stopped);
    appendList(out, "Rejoin", rejoin);
    appendList(out, "Test", test);
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
