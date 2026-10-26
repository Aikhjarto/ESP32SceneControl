// Scene engine: one scene clock drives every servo, so the choreography stays in sync.
//
// A servo's target is a pure function of scene time. Global stop freezes the clock.
// A per-servo stop only freezes that servo's output while its target moves on; on Go the
// servo blends smoothly back onto its moving target ("rejoin"). The same blend is used
// after a scene reset, a test move, re-enabling, or editing a setting while running.
//
// Not thread-safe: the caller serializes access (the firmware uses one config/scene lock).
#pragma once

#include <cstdint>
#include <string>

#include "core/Config.h"

namespace ss {

class Scene {
public:
    explicit Scene(const Config& cfg) : cfg_(cfg) {
        for (bool& p : present_) p = true;
        for (bool& p : outPresent_) p = true;
    }

    // Advance by dtUs of real time. Scene time only advances while running.
    void tick(int64_t dtUs);

    bool running() const { return running_; }
    void setRunning(bool run) { running_ = run; }
    void reset();  // scene time back to 0, servos rejoin the start of the choreography

    void setServoRun(int servo, bool run);  // servo < 0: all servos
    bool servoStopped(int servo) const { return st_[servo].stopped; }

    // Calibration: drive a servo to an angle (speed-limited by max_dps) until released.
    void test(int servo, float deg);
    void release(int servo);  // servo < 0: all servos
    bool testing(int servo) const { return st_[servo].testing; }

    // Settle smoothly from the current output (keeping its speed) onto the target, e.g.
    // after a setting changed. servo < 0: all servos.
    void rejoin(int servo);
    bool rejoining(int servo) const { return st_[servo].rejoining; }

    bool hasOutput(int servo) const { return st_[servo].valid && st_[servo].active; }

    // Whether a servo has working hardware (set by the firmware after allocating outputs).
    // Servos without hardware are ignored, also for the automatic scene period.
    void setPresent(int servo, bool present) { present_[servo] = present; }
    float output(int servo) const { return st_[servo].out; }

    // Digital outputs follow their switch-point sequence on the same scene clock. Global
    // Stop holds them; a per-output Stop holds one while the scene runs on; a test override
    // sets one by hand until released.
    bool outputOn(int out) const { return ost_[out].on; }
    bool hasOutputState(int out) const { return ost_[out].active; }
    void setOutputRun(int out, bool run);  // out < 0: all outputs
    bool outputStopped(int out) const { return ost_[out].stopped; }
    void outputTest(int out, bool on);
    void outputRelease(int out);  // out < 0: all outputs
    bool outputTesting(int out) const { return ost_[out].testing; }
    void setOutputPresent(int out, bool present) { outPresent_[out] = present; }
    // "[OUT:0=1,3=0]" for every output with a state
    std::string outputs() const;

    int64_t timeUs() const { return tUs_; }
    uint32_t periodMs() const { return periodMs_; }
    uint32_t cycle() const { return periodMs_ ? static_cast<uint32_t>(tUs_ / (periodMs_ * 1000LL)) : 0; }

    // "<Run|T:12.340|Cyc:3|Stop:3,7|Rejoin:5|Test:2|OStop:1|OTest:4>"
    std::string status() const;
    // "[POS:0=45.0,1=90.5,...]" for every servo with an output
    std::string positions() const;

private:
    struct ServoState {
        bool valid = false;   // out holds a real position
        bool active = false;  // servo enabled and wired at the last tick
        bool stopped = false;
        bool testing = false;
        bool rejoining = false;  // settling onto the target (see startRejoin)
        float out = 0;
        float vel = 0;  // output speed over the last tick, deg/s
        float testDeg = 0;
        float e0 = 0;   // settle: position offset at t0
        float e1 = 0;   // settle: speed offset at t0, deg/s
        int64_t t0Us = 0;
        int64_t durUs = 0;
    };

    struct OutputState {
        bool active = false;  // enabled, wired and with hardware at the last tick
        bool stopped = false;
        bool testing = false;
        bool on = false;
        bool testOn = false;
    };

    bool live(int servo) const { return present_[servo] && servoActive(cfg_.servo[servo]); }
    bool outLive(int out) const { return outPresent_[out] && outputActive(cfg_.out[out]); }
    void startRejoin(int servo);
    int64_t blendDurationUs(const ServoConfig& s, float distDeg) const;
    float targetVelocity(const ServoConfig& s, int64_t tUs) const;

    const Config& cfg_;
    ServoState st_[kMaxServos];
    bool present_[kMaxServos];
    OutputState ost_[kMaxOutputs];
    bool outPresent_[kMaxOutputs];
    int64_t tUs_ = 0;
    int64_t outTimeUs_ = 0;  // scene time the current outputs belong to
    int64_t outDtUs_ = 0;    // scene time step that produced them
    uint32_t periodMs_ = 0;
    bool running_ = false;
};

}  // namespace ss
