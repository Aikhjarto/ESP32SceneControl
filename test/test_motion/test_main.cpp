// Native unit tests for the motion math and the scene engine.
#include <unity.h>

#include <cmath>

#include "core/Config.h"
#include "core/Motion.h"
#include "core/Scene.h"

using namespace ss;

static Config cfg;

void setUp() {
    setDefaults(cfg);
    // One servo is enough for most tests; the rest are switched off.
    for (int i = 1; i < kMaxServos; ++i) cfg.servo[i].enable = false;
    ServoConfig& s = cfg.servo[0];
    s.startDeg = 0;
    s.stopDeg = 90;
    s.delayMs = 0;
    s.durationMs = 1000;
    s.dwellMs = 500;
    s.mode = Mode::Return;
    cfg.scene.periodMs = 4000;
    cfg.scene.rejoinMs = 500;
}

void tearDown() {}

static const Profile kProfiles[] = {Profile::Const, Profile::Trap, Profile::Tri, Profile::Sine, Profile::Bell};

void test_profiles_hit_endpoints_and_are_monotonic() {
    for (Profile p : kProfiles) {
        TEST_ASSERT_EQUAL_FLOAT(0, profilePos(p, 0, 25));
        TEST_ASSERT_EQUAL_FLOAT(1, profilePos(p, 1, 25));
        float prev = 0;
        for (int k = 1; k <= 1000; ++k) {
            float s = profilePos(p, k / 1000.0f, 25);
            TEST_ASSERT_TRUE(s >= prev - 1e-6f);
            prev = s;
        }
    }
}

void test_profiles_are_continuous_and_symmetric() {
    for (Profile p : kProfiles) {
        for (int k = 0; k < 1000; ++k) {
            float u = k / 1000.0f;
            float a = profilePos(p, u, 25), b = profilePos(p, u + 0.001f, 25);
            TEST_ASSERT_TRUE(std::fabs(b - a) < 0.0025f);  // no jumps (peak slope ≤ 2)
            TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1 - a, profilePos(p, 1 - u, 25));
        }
    }
}

void test_peak_factor_matches_numeric_slope() {
    for (Profile p : kProfiles) {
        float peak = 0;
        const float h = 1e-4f;
        for (int k = 0; k < 10000; ++k) {
            float u = k / 10000.0f;
            peak = std::fmax(peak, (profilePos(p, u + h, 25) - profilePos(p, u, 25)) / h);
        }
        TEST_ASSERT_FLOAT_WITHIN(0.01f, profilePeakFactor(p, 25), peak);
    }
}

void test_trapezoid_extremes_equal_const_and_tri() {
    for (int k = 0; k <= 100; ++k) {
        float u = k / 100.0f;
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, profilePos(Profile::Const, u, 0), profilePos(Profile::Trap, u, 0));
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, profilePos(Profile::Tri, u, 0), profilePos(Profile::Trap, u, 50));
    }
}

void test_return_mode_timeline() {
    const ServoConfig& s = cfg.servo[0];
    TEST_ASSERT_EQUAL_FLOAT(0, targetDeg(s, 0, 4000));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 45, targetDeg(s, 500000, 4000));  // bell is symmetric
    TEST_ASSERT_EQUAL_FLOAT(90, targetDeg(s, 1200000, 4000));         // dwell
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 45, targetDeg(s, 2000000, 4000));  // halfway back
    TEST_ASSERT_EQUAL_FLOAT(0, targetDeg(s, 3000000, 4000));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 45, targetDeg(s, 4500000, 4000));  // next cycle
}

void test_delay_shifts_the_move() {
    ServoConfig s = cfg.servo[0];
    s.delayMs = 1000;
    TEST_ASSERT_EQUAL_FLOAT(0, targetDeg(s, 900000, 4000));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 45, targetDeg(s, 1500000, 4000));
}

void test_once_mode_holds_after_first_cycle() {
    ServoConfig s = cfg.servo[0];
    s.mode = Mode::Once;
    TEST_ASSERT_EQUAL_FLOAT(90, targetDeg(s, 2000000, 4000));
    TEST_ASSERT_EQUAL_FLOAT(90, targetDeg(s, 9000000, 4000));
}

void test_pingpong_runs_independent_of_period() {
    ServoConfig s = cfg.servo[0];
    s.mode = Mode::PingPong;  // cycle = 2*1000 + 2*500 = 3000 ms
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 45, targetDeg(s, 500000, 4000));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 45, targetDeg(s, 3500000, 4000));
    TEST_ASSERT_EQUAL_FLOAT(0, targetDeg(s, 2800000, 4000));
}

void test_auto_period_is_longest_servo() {
    cfg.scene.periodMs = 0;
    TEST_ASSERT_EQUAL_UINT32(2500, effectivePeriodMs(cfg));
    cfg.servo[1].enable = true;
    cfg.servo[1].delayMs = 3000;
    TEST_ASSERT_EQUAL_UINT32(3000 + 2 * 1500 + 500, effectivePeriodMs(cfg));
}

void test_pulse_mapping() {
    ServoConfig s = cfg.servo[0];
    TEST_ASSERT_EQUAL_FLOAT(500, pulseUs(s, 0));
    TEST_ASSERT_EQUAL_FLOAT(1450, pulseUs(s, 90));
    TEST_ASSERT_EQUAL_FLOAT(2400, pulseUs(s, 200));  // clamped to range
    s.invert = true;
    TEST_ASSERT_EQUAL_FLOAT(2400, pulseUs(s, 0));
}

static void run(Scene& sc, int64_t ms) {
    for (int64_t t = 0; t < ms; t += 20) sc.tick(20000);
}

void test_global_hold_freezes_everything() {
    Scene sc(cfg);
    sc.setRunning(true);
    run(sc, 300);
    float p = sc.output(0);
    sc.setRunning(false);
    run(sc, 1000);
    TEST_ASSERT_EQUAL_FLOAT(p, sc.output(0));
    TEST_ASSERT_EQUAL_INT64(300000, sc.timeUs());
}

void test_servo_stop_holds_while_scene_moves_on() {
    Scene sc(cfg);
    sc.setRunning(true);
    run(sc, 300);
    float held = sc.output(0);
    sc.setServoRun(0, false);
    run(sc, 400);
    TEST_ASSERT_EQUAL_FLOAT(held, sc.output(0));
    TEST_ASSERT_EQUAL_INT64(700000, sc.timeUs());  // the choreography kept running
}

// After Go the servo blends back: no jump, and it lands exactly on the moving target
// with matching speed, i.e. it is back in sync with the rest of the scene.
void test_rejoin_is_smooth_and_resyncs() {
    Scene sc(cfg);
    sc.setRunning(true);
    run(sc, 200);
    sc.setServoRun(0, false);
    run(sc, 600);
    float before = sc.output(0);
    sc.setServoRun(0, true);
    TEST_ASSERT_TRUE(sc.rejoining(0));

    sc.tick(20000);
    TEST_ASSERT_TRUE(std::fabs(sc.output(0) - before) < 1.0f);  // starts from standstill

    float prev = sc.output(0);
    float maxStep = 0;
    for (int k = 0; k < 200 && sc.rejoining(0); ++k) {
        sc.tick(20000);
        maxStep = std::fmax(maxStep, std::fabs(sc.output(0) - prev));
        prev = sc.output(0);
    }
    TEST_ASSERT_FALSE(sc.rejoining(0));
    TEST_ASSERT_TRUE(maxStep < 600 * 0.02f * 1.5f);  // stays near the servo's speed limit
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, targetDeg(cfg.servo[0], sc.timeUs(), 4000), sc.output(0));
}

void test_rejoin_is_frozen_by_global_hold() {
    Scene sc(cfg);
    sc.setRunning(true);
    run(sc, 200);
    sc.setServoRun(0, false);
    run(sc, 600);
    sc.setRunning(false);
    float held = sc.output(0);
    sc.setServoRun(0, true);
    run(sc, 2000);
    TEST_ASSERT_EQUAL_FLOAT(held, sc.output(0));
    TEST_ASSERT_TRUE(sc.rejoining(0));
}

void test_test_mode_is_speed_limited_and_releases_with_rejoin() {
    Scene sc(cfg);
    sc.tick(20000);
    sc.test(0, 180);
    sc.tick(100000);  // 0.1 s at 600 deg/s → 60 deg
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 60, sc.output(0));
    run(sc, 1000);
    TEST_ASSERT_EQUAL_FLOAT(180, sc.output(0));
    sc.release(0);
    TEST_ASSERT_TRUE(sc.rejoining(0));
}

// With the automatic period, editing or disabling one servo changes the cycle length for
// everyone; the other servos must blend over instead of jumping (review finding).
void test_period_change_does_not_make_other_servos_jump() {
    cfg.scene.periodMs = 0;
    cfg.servo[1].enable = true;
    cfg.servo[1].delayMs = 3000;  // period = 3000 + 2*1500 + 500 = 6500 ms
    Scene sc(cfg);
    sc.setRunning(true);
    run(sc, 20000);
    const float maxStep = 600 * 0.02f * 1.5f;  // servo speed limit per tick, with margin

    for (auto edit : {+1, 0}) {
        if (edit) cfg.servo[1].delayMs = 3500;  // period → 7000 ms
        else cfg.servo[1].enable = false;       // period → servo 0's 2500 ms
        float prev = sc.output(0);
        for (int k = 0; k < 200; ++k) {
            sc.tick(20000);
            TEST_ASSERT_TRUE(std::fabs(sc.output(0) - prev) < maxStep);
            prev = sc.output(0);
        }
        TEST_ASSERT_FALSE(sc.rejoining(0));
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, targetDeg(cfg.servo[0], sc.timeUs(), sc.periodMs()), sc.output(0));
    }
}

void test_status_lists_states() {
    Scene sc(cfg);
    cfg.servo[3].enable = true;
    sc.setRunning(true);
    run(sc, 100);
    sc.setServoRun(3, false);
    sc.test(0, 10);
    TEST_ASSERT_EQUAL_STRING("<Run|T:0.100|Cyc:0|Stop:3|Test:0>", sc.status().c_str());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_profiles_hit_endpoints_and_are_monotonic);
    RUN_TEST(test_profiles_are_continuous_and_symmetric);
    RUN_TEST(test_peak_factor_matches_numeric_slope);
    RUN_TEST(test_trapezoid_extremes_equal_const_and_tri);
    RUN_TEST(test_return_mode_timeline);
    RUN_TEST(test_delay_shifts_the_move);
    RUN_TEST(test_once_mode_holds_after_first_cycle);
    RUN_TEST(test_pingpong_runs_independent_of_period);
    RUN_TEST(test_auto_period_is_longest_servo);
    RUN_TEST(test_pulse_mapping);
    RUN_TEST(test_global_hold_freezes_everything);
    RUN_TEST(test_servo_stop_holds_while_scene_moves_on);
    RUN_TEST(test_rejoin_is_smooth_and_resyncs);
    RUN_TEST(test_rejoin_is_frozen_by_global_hold);
    RUN_TEST(test_test_mode_is_speed_limited_and_releases_with_rejoin);
    RUN_TEST(test_period_change_does_not_make_other_servos_jump);
    RUN_TEST(test_status_lists_states);
    return UNITY_END();
}
