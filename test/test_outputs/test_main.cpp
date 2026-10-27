// Native unit tests for digital outputs: switch-point sequences, scene behaviour, settings
// and commands.
#include <unity.h>

#include <string>
#include <vector>

#include "core/Config.h"
#include "core/Parser.h"
#include "core/Scene.h"
#include "core/Sequence.h"

using namespace ss;

struct FakeHost : Host {
    Scene* scene = nullptr;
    void setRunning(bool run, const char*) override { scene->setRunning(run); }
    bool save(const std::string&, std::string&) override { return true; }
    bool erase(std::string&) override { return true; }
    void reboot() override {}
    void info(ReplySink&) override {}
};

struct Capture : ReplySink {
    std::vector<std::string> lines;
    void line(const std::string& t) override { lines.push_back(t); }
    std::string last() const { return lines.empty() ? "" : lines.back(); }
    bool hasPrefix(const std::string& p) const {
        for (const auto& l : lines) {
            if (l.rfind(p, 0) == 0) return true;
        }
        return false;
    }
};

static Config cfg;
static Scene* scene;
static FakeHost* host;
static Parser* parser;

void setUp() {
    setDefaults(cfg);
    for (int i = 1; i < kMaxServos; ++i) cfg.servo[i].enable = false;  // one servo: 3500 ms auto period
    cfg.servo[0].delayMs = 0;
    scene = new Scene(cfg);
    host = new FakeHost;
    host->scene = scene;
    parser = new Parser(cfg, *scene, *host);
}

void tearDown() {
    delete parser;
    delete host;
    delete scene;
}

static Capture exec(const std::string& line) {
    Capture c;
    parser->execute(line, c);
    return c;
}

static void run(int64_t ms) {
    for (int64_t t = 0; t < ms; t += 20) scene->tick(20000);
}

void test_parse_sequence() {
    Sequence s;
    std::string err;
    TEST_ASSERT_TRUE(parseSequence("0:on, 500:off,1200:ON,1500:0", s, err));
    TEST_ASSERT_EQUAL(4, s.count);
    TEST_ASSERT_EQUAL_UINT32(1200, s.timeMs[2]);
    TEST_ASSERT_TRUE(s.on[2]);
    TEST_ASSERT_FALSE(s.on[3]);
    TEST_ASSERT_TRUE(parseSequence("", s, err));
    TEST_ASSERT_EQUAL(0, s.count);

    for (const char* bad : {"500:off,100:on", "0:maybe", "x:on", "0:on,0:off", "4000000:on", "0 on"}) {
        err.clear();
        TEST_ASSERT_FALSE(parseSequence(bad, s, err));
        TEST_ASSERT_FALSE(err.empty());
    }
    std::string many;
    for (int k = 0; k <= kMaxSwitchPoints; ++k) many += std::to_string(k * 10) + ":on,";
    TEST_ASSERT_FALSE(parseSequence(many.c_str(), s, err));
}

// The longest valid sequence (16 points with 7-digit times) must fit the setting.
void test_longest_sequence_fits() {
    std::string seq;
    for (int k = 0; k < kMaxSwitchPoints; ++k) {
        seq += (k ? "," : "") + std::to_string(3600000 - (kMaxSwitchPoints - 1 - k)) + (k % 2 ? ":off" : ":on");
    }
    exec("$/out/0/pin=gpio.12");
    TEST_ASSERT_EQUAL_STRING("ok", exec("$/out/0/seq=" + seq).last().c_str());
    TEST_ASSERT_EQUAL_STRING(seq.c_str(), cfg.out[0].seq);
}

void test_sequence_state_wraps() {
    Sequence s;
    std::string err;
    parseSequence("500:on,1200:off", s, err);
    TEST_ASSERT_FALSE(sequenceState(s, 0));    // before the first point: state of the last point
    TEST_ASSERT_FALSE(sequenceState(s, 499));
    TEST_ASSERT_TRUE(sequenceState(s, 500));
    TEST_ASSERT_TRUE(sequenceState(s, 1199));
    TEST_ASSERT_FALSE(sequenceState(s, 1200));
    parseSequence("500:off,1200:on", s, err);
    TEST_ASSERT_TRUE(sequenceState(s, 100));   // still on from the end of the previous cycle
    Sequence empty;
    TEST_ASSERT_FALSE(sequenceState(empty, 100));
    // Sixth review: a point after the end of the cycle is never reached and sets no state.
    parseSequence("200:on,600:off,1500:on", s, err);
    TEST_ASSERT_FALSE(sequenceState(s, 100, 1000));
    TEST_ASSERT_TRUE(sequenceState(s, 300, 1000));
    TEST_ASSERT_FALSE(sequenceState(s, 700, 1000));
    TEST_ASSERT_TRUE(sequenceState(s, 100, 2000));  // reached with a longer cycle
    parseSequence("1500:on", s, err);
    TEST_ASSERT_FALSE(sequenceState(s, 100, 1000));
}

static void wireOutput(int o, const char* seq) {
    TEST_ASSERT_EQUAL_STRING("ok", exec("$/out/" + std::to_string(o) + "/pin=gpio.12").last().c_str());
    exec("$/out/" + std::to_string(o) + "/enable=true");
    TEST_ASSERT_EQUAL_STRING("ok", exec("$/out/" + std::to_string(o) + "/seq=" + seq).last().c_str());
}

void test_output_follows_scene_clock() {
    wireOutput(2, "0:on,1000:off");
    scene->setRunning(true);
    run(500);
    TEST_ASSERT_TRUE(scene->outputOn(2));
    run(600);  // 1.1 s
    TEST_ASSERT_FALSE(scene->outputOn(2));
    run(2500);  // 3.6 s: next cycle (period 1500 + 500 + 1500 = 3500 ms)
    TEST_ASSERT_TRUE(scene->outputOn(2));
    TEST_ASSERT_EQUAL_STRING("[OUT:2=1]", scene->outputs().c_str());
}

void test_global_hold_and_output_stop() {
    wireOutput(0, "0:on,1000:off");
    scene->setRunning(true);
    run(500);
    scene->setRunning(false);
    run(2000);
    TEST_ASSERT_TRUE(scene->outputOn(0));  // frozen with the scene clock
    scene->setRunning(true);

    TEST_ASSERT_EQUAL_STRING("ok", exec("$Output/Stop=0").last().c_str());
    run(1000);  // scene passes 1000 ms, but the output holds
    TEST_ASSERT_TRUE(scene->outputOn(0));
    TEST_ASSERT_TRUE(scene->status().find("OStop:0") != std::string::npos);
    TEST_ASSERT_EQUAL_STRING("ok", exec("$Output/Go=*").last().c_str());
    run(20);
    TEST_ASSERT_FALSE(scene->outputOn(0));  // takes the scene's state again
}

void test_output_test_override() {
    wireOutput(1, "0:off");
    scene->setRunning(true);
    run(100);
    TEST_ASSERT_EQUAL_STRING("ok", exec("$Output/Set=1,on").last().c_str());
    run(20);
    TEST_ASSERT_TRUE(scene->outputOn(1));
    TEST_ASSERT_TRUE(scene->status().find("OTest:1") != std::string::npos);
    TEST_ASSERT_EQUAL_STRING("error:2", exec("$Output/Set=1,dim").last().c_str());
    TEST_ASSERT_EQUAL_STRING("error:6", exec("$Output/Set=9,on").last().c_str());
    TEST_ASSERT_EQUAL_STRING("ok", exec("$Output/Release=1").last().c_str());
    run(20);
    TEST_ASSERT_FALSE(scene->outputOn(1));
}

void test_output_without_hardware_is_ignored() {
    wireOutput(3, "0:on");
    scene->setOutputPresent(3, false);
    run(100);
    TEST_ASSERT_FALSE(scene->hasOutputState(3));
    TEST_ASSERT_EQUAL_STRING("[OUT:]", scene->outputs().c_str());
}

void test_pin_rules_and_settings() {
    Capture bad = exec("$/out/0/pin=pca.0.1");
    TEST_ASSERT_EQUAL_STRING("error:2", bad.last().c_str());
    TEST_ASSERT_EQUAL_STRING("error:2", exec("$/out/0/pin=pcx.0.1").last().c_str());
    TEST_ASSERT_TRUE(exec("$/out/0/pin=pcx.0.1").hasPrefix("[MSG:ERR: expected none, gpio.N or pcf.B.C"));
    TEST_ASSERT_EQUAL_STRING("ok", exec("$/out/0/pin=pcf.7.15").last().c_str());
    TEST_ASSERT_EQUAL_STRING("$/out/0/pin=pcf.7.15", exec("$/out/0/pin").lines[0].c_str());
    TEST_ASSERT_EQUAL_STRING("error:2", exec("$/servo/0/pin=pcf.0.1").last().c_str());
    TEST_ASSERT_EQUAL_STRING("error:2", exec("$/out/0/pin=pcf.8.0").last().c_str());
    TEST_ASSERT_EQUAL_STRING("error:2", exec("$/i2c/sda=pcf.0.1").last().c_str());
    TEST_ASSERT_EQUAL_STRING("error:2", exec("$/out/0/seq=500:off,100:on").last().c_str());
    TEST_ASSERT_EQUAL_STRING("ok", exec("$/out/0/name=Lantern").last().c_str());
    TEST_ASSERT_EQUAL_STRING("ok", exec("$/pcf/1/type=pcf8575").last().c_str());
    TEST_ASSERT_EQUAL_STRING("error:4", exec("$/pcf/1/addr=0x40").last().c_str());
    TEST_ASSERT_EQUAL_STRING("ok", exec("$/pcf/1/addr=0x38").last().c_str());
}

void test_listing_hides_unwired_outputs() {
    wireOutput(5, "0:on");
    Capture all = exec("$$");
    TEST_ASSERT_TRUE(all.hasPrefix("$/out/5/seq=0:on"));
    TEST_ASSERT_FALSE(all.hasPrefix("$/out/6/"));
    TEST_ASSERT_TRUE(all.hasPrefix("$/pcf/0/type=pcf8574"));
    Capture one = exec("$/out/6");  // explicitly asked for
    TEST_ASSERT_EQUAL(5 + 1, static_cast<int>(one.lines.size()));
}

void test_unreached_switch_points_warn() {
    exec("$/out/0/pin=gpio.12");
    exec("$/out/0/enable=true");
    exec("$/scene/period_ms=3500");
    Capture c = exec("$/out/0/seq=0:on,4000:off");  // beyond the fixed 3500 ms period
    TEST_ASSERT_TRUE(c.hasPrefix("[MSG:WARN: outputs 0 have switch points after the end of the 3500 ms"));
    TEST_ASSERT_EQUAL_STRING("ok", c.last().c_str());
    TEST_ASSERT_FALSE(exec("$/out/0/name=Lamp").hasPrefix("[MSG:WARN: outputs"));  // only sequence edits warn

    // Automatic period: the cycle grows to cover the last switch point (rounded up to a second).
    exec("$/scene/period_ms=0");
    TEST_ASSERT_FALSE(exec("$/out/0/seq=0:on,4200:off").hasPrefix("[MSG:WARN: outputs"));
    TEST_ASSERT_EQUAL_UINT32(5000, scene->effectivePeriod());
}

// Fourth review: while the scene is held, outputs keep their state; only Force/Release and
// per-output Go act. $RST=* also clears per-output stop and force.
void test_held_scene_holds_outputs() {
    wireOutput(0, "0:on,1000:off");
    scene->setRunning(true);
    run(500);
    scene->setRunning(false);
    exec("$/out/0/seq=0:off");  // edit while held: no change yet
    run(100);
    TEST_ASSERT_TRUE(scene->outputOn(0));
    exec("$Output/Set=0,off");
    run(20);
    TEST_ASSERT_FALSE(scene->outputOn(0));
    exec("$Output/Release=0");  // back to the (edited) sequence at the frozen time
    run(20);
    TEST_ASSERT_FALSE(scene->outputOn(0));
    exec("$/out/0/seq=0:on");
    scene->setRunning(true);
    run(20);
    TEST_ASSERT_TRUE(scene->outputOn(0));  // Go continues with the sequence
}

void test_reset_defaults_clears_output_states() {
    wireOutput(0, "0:on");
    exec("$Output/Stop=0");
    exec("$Output/Set=0,off");
    TEST_ASSERT_EQUAL_STRING("ok", exec("$RST=*").last().c_str());
    TEST_ASSERT_FALSE(scene->outputStopped(0));
    TEST_ASSERT_FALSE(scene->outputTesting(0));
}

void test_outputs_survive_save_and_restore() {
    wireOutput(4, "100:on,900:off");
    exec("$/out/4/name=Lantern");
    exec("$/out/4/invert=true");
    std::string saved = parser->settings().dumpChanged();
    setDefaults(cfg);
    Capture c;
    TEST_ASSERT_TRUE(parser->restore(saved, c));
    TEST_ASSERT_EQUAL_STRING("100:on,900:off", cfg.out[4].seq);
    TEST_ASSERT_EQUAL_STRING("Lantern", cfg.out[4].name);
    TEST_ASSERT_TRUE(cfg.out[4].invert);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_sequence);
    RUN_TEST(test_longest_sequence_fits);
    RUN_TEST(test_sequence_state_wraps);
    RUN_TEST(test_output_follows_scene_clock);
    RUN_TEST(test_global_hold_and_output_stop);
    RUN_TEST(test_output_test_override);
    RUN_TEST(test_output_without_hardware_is_ignored);
    RUN_TEST(test_pin_rules_and_settings);
    RUN_TEST(test_listing_hides_unwired_outputs);
    RUN_TEST(test_unreached_switch_points_warn);
    RUN_TEST(test_held_scene_holds_outputs);
    RUN_TEST(test_reset_defaults_clears_output_states);
    RUN_TEST(test_outputs_survive_save_and_restore);
    return UNITY_END();
}
