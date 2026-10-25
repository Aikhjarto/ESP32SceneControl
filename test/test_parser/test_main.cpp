// Native unit tests for the settings tree and the FluidNC-style command parser.
#include <unity.h>

#include <string>
#include <vector>

#include "core/Config.h"
#include "core/Parser.h"
#include "core/Scene.h"

using namespace ss;

struct FakeHost : Host {
    Scene* scene = nullptr;
    std::string saved;
    bool erased = false;
    bool rebooted = false;
    void setRunning(bool run, const char*) override { scene->setRunning(run); }
    bool save(const std::string& text, std::string&) override { return saved = text, true; }
    bool erase(std::string&) override { return erased = true, true; }
    void reboot() override { rebooted = true; }
    void info(ReplySink& out) override { out.line("[VER:test]"); }
    Err command(const std::string& name, const std::string& value, bool, ReplySink& out, std::string&) override {
        if (name != "Firmware/Update") return Err::UnknownCommand;
        out.line("[OTA:start " + value + "]");
        return Err::Ok;
    }
};

struct Capture : ReplySink {
    std::vector<std::string> lines;
    void line(const std::string& t) override { lines.push_back(t); }
    std::string last() const { return lines.empty() ? "" : lines.back(); }
    bool has(const std::string& t) const {
        for (const auto& l : lines) {
            if (l == t) return true;
        }
        return false;
    }
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

void test_set_and_query_tree_setting() {
    TEST_ASSERT_EQUAL_STRING("ok", exec("$/servo/3/start_deg=20").last().c_str());
    TEST_ASSERT_EQUAL_FLOAT(20, cfg.servo[3].startDeg);
    Capture c = exec("$/servo/3/start_deg");
    TEST_ASSERT_EQUAL_STRING("$/servo/3/start_deg=20", c.lines[0].c_str());
    TEST_ASSERT_EQUAL_STRING("ok", c.last().c_str());
}

void test_paths_are_case_insensitive_and_tolerate_spaces() {
    TEST_ASSERT_EQUAL_STRING("ok", exec("  $/Servo/3/Profile = TRAP ").last().c_str());
    TEST_ASSERT_EQUAL(static_cast<int>(Profile::Trap), static_cast<int>(cfg.servo[3].profile));
}

void test_errors() {
    TEST_ASSERT_EQUAL_STRING("error:5", exec("$/servo/3/nope=1").last().c_str());
    TEST_ASSERT_EQUAL_STRING("error:5", exec("$/servo/64/start_deg=1").last().c_str());
    TEST_ASSERT_EQUAL_STRING("error:2", exec("$/servo/3/start_deg=abc").last().c_str());
    TEST_ASSERT_EQUAL_STRING("error:4", exec("$/servo/3/start_deg=400").last().c_str());
    TEST_ASSERT_EQUAL_STRING("error:2", exec("$/servo/3/mode=bounce").last().c_str());
    TEST_ASSERT_EQUAL_STRING("error:3", exec("$Frobnicate").last().c_str());
    TEST_ASSERT_EQUAL_STRING("error:3", exec("G0 X10").last().c_str());
    Capture c = exec("$/servo/3/start_deg=400");
    TEST_ASSERT_EQUAL_STRING("[MSG:ERR: allowed range 0..360]", c.lines[0].c_str());
}

void test_pins_and_enums_roundtrip() {
    TEST_ASSERT_EQUAL_STRING("ok", exec("$/servo/40/pin=pca.1.7").last().c_str());
    TEST_ASSERT_EQUAL_STRING("$/servo/40/pin=pca.1.7", exec("$/servo/40/pin").lines[0].c_str());
    TEST_ASSERT_EQUAL_STRING("error:2", exec("$/button/pin=pca.0.1").last().c_str());
    TEST_ASSERT_EQUAL_STRING("ok", exec("$/pca/1/addr=0x41").last().c_str());
    TEST_ASSERT_EQUAL_STRING("$/pca/1/addr=0x41", exec("$/pca/1/addr").lines[0].c_str());
}

void test_reboot_settings_say_so() {
    Capture c = exec("$/servo/0/pin=gpio.12");
    TEST_ASSERT_TRUE(c.hasPrefix("[MSG:INFO: Restart required"));
    TEST_ASSERT_EQUAL_STRING("ok", c.last().c_str());
}

void test_speed_warning() {
    Capture c = exec("$/servo/0/duration_ms=100");  // 90 deg in 0.1 s with bell → 1687 deg/s
    TEST_ASSERT_TRUE(c.hasPrefix("[MSG:WARN: servo 0 would need"));
}

void test_named_settings_and_secret_masking() {
    TEST_ASSERT_EQUAL_STRING("ok", exec("$ap/ssid=Puppets").last().c_str());
    TEST_ASSERT_EQUAL_STRING("Puppets", cfg.net.ssid);
    TEST_ASSERT_EQUAL_STRING("error:2", exec("$AP/Password=short").last().c_str());
    TEST_ASSERT_EQUAL_STRING("ok", exec("$AP/Password=longenough").last().c_str());
    TEST_ASSERT_EQUAL_STRING("$AP/Password=********", exec("$AP/Password").lines[0].c_str());
    TEST_ASSERT_EQUAL_STRING("error:2", exec("$AP/IP=10.0.0").last().c_str());
}

void test_subtree_listing() {
    Capture c = exec("$/servo/2");
    TEST_ASSERT_EQUAL(18 + 1, static_cast<int>(c.lines.size()));
    TEST_ASSERT_EQUAL_STRING("$/servo/2/name=", c.lines[0].c_str());
    Capture all = exec("$$");
    TEST_ASSERT_TRUE(all.has("$/scene/period_ms=0"));
    TEST_ASSERT_TRUE(all.has("$AP/Channel=6"));
    TEST_ASSERT_FALSE(all.hasPrefix("$/servo/40/"));  // unwired slots are hidden
    TEST_ASSERT_TRUE(all.hasPrefix("$/servo/31/"));
}

void test_save_writes_only_changes_and_replay_restores() {
    exec("$/servo/5/stop_deg=170");
    exec("$/servo/5/profile=sine");
    exec("$AP/SSID=Stage");
    exec("$/servo/50/pin=gpio.12");
    Capture c = exec("$CS");
    TEST_ASSERT_EQUAL_STRING("ok", c.last().c_str());
    TEST_ASSERT_EQUAL_STRING(
        "$/servo/5/stop_deg=170\n$/servo/5/profile=sine\n$/servo/50/pin=gpio.12\n$AP/SSID=Stage\n",
        host->saved.c_str());

    setDefaults(cfg);
    Capture r;
    parser->replay(host->saved, r);
    TEST_ASSERT_TRUE(r.lines.empty());
    TEST_ASSERT_EQUAL_FLOAT(170, cfg.servo[5].stopDeg);
    TEST_ASSERT_EQUAL_STRING("Stage", cfg.net.ssid);
    TEST_ASSERT_EQUAL(static_cast<int>(PinType::Gpio), static_cast<int>(cfg.servo[50].pin.type));
}

void test_replay_reports_bad_lines() {
    Capture r;
    parser->replay("$/servo/1/start_deg=10\n$/servo/1/bogus=1\n", r);
    TEST_ASSERT_EQUAL(1, static_cast<int>(r.lines.size()));
    TEST_ASSERT_EQUAL_STRING("[MSG:WARN: Config line failed: $/servo/1/bogus=1 (unknown setting: $/servo/1/bogus)]",
                             r.lines[0].c_str());
    TEST_ASSERT_EQUAL_FLOAT(10, cfg.servo[1].startDeg);
}

void test_restore_defaults() {
    exec("$/servo/1/start_deg=10");
    TEST_ASSERT_EQUAL_STRING("error:2", exec("$RST").last().c_str());
    TEST_ASSERT_EQUAL_STRING("ok", exec("$RST=*").last().c_str());
    TEST_ASSERT_EQUAL_FLOAT(45, cfg.servo[1].startDeg);
    TEST_ASSERT_TRUE(host->erased);
}

void test_scene_and_servo_commands() {
    TEST_ASSERT_EQUAL_STRING("ok", exec("$Scene/Go").last().c_str());
    TEST_ASSERT_TRUE(scene->running());
    scene->tick(20000);
    TEST_ASSERT_EQUAL_STRING("ok", exec("$Servo/Stop=4").last().c_str());
    TEST_ASSERT_TRUE(scene->servoStopped(4));
    TEST_ASSERT_EQUAL_STRING("ok", exec("$Servo/Go=*").last().c_str());
    TEST_ASSERT_FALSE(scene->servoStopped(4));
    TEST_ASSERT_EQUAL_STRING("error:6", exec("$Servo/Stop=60").last().c_str());
    TEST_ASSERT_EQUAL_STRING("ok", exec("$Servo/Test=2,90").last().c_str());
    TEST_ASSERT_TRUE(scene->testing(2));
    TEST_ASSERT_EQUAL_STRING("error:4", exec("$Servo/Test=2,190").last().c_str());
    TEST_ASSERT_EQUAL_STRING("error:2", exec("$Servo/Test=2").last().c_str());
    TEST_ASSERT_EQUAL_STRING("ok", exec("$Servo/Release=2").last().c_str());
    TEST_ASSERT_FALSE(scene->testing(2));
}

void test_realtime_characters() {
    Capture c;
    parser->realtime('~', c);
    TEST_ASSERT_TRUE(scene->running());
    parser->realtime('!', c);
    TEST_ASSERT_FALSE(scene->running());
    parser->realtime('?', c);
    TEST_ASSERT_EQUAL_STRING("<Hold|T:0.000|Cyc:0>", c.last().c_str());
}

void test_host_commands_extend_the_parser() {
    Capture c = exec("$Firmware/Update=http://x/fw.bin");
    TEST_ASSERT_EQUAL_STRING("[OTA:start http://x/fw.bin]", c.lines[0].c_str());
    TEST_ASSERT_EQUAL_STRING("ok", c.last().c_str());
    TEST_ASSERT_EQUAL_STRING("error:3", exec("$Firmware/Nope").last().c_str());
}

void test_empty_line_and_help() {
    TEST_ASSERT_EQUAL_STRING("ok", exec("").last().c_str());
    Capture h = exec("$");
    TEST_ASSERT_TRUE(h.hasPrefix("[HLP:"));
}

void test_yaml_dump() {
    Capture c = exec("$CD");
    TEST_ASSERT_EQUAL_STRING("servo:", c.lines[0].c_str());
    TEST_ASSERT_EQUAL_STRING("  0:", c.lines[1].c_str());
    TEST_ASSERT_EQUAL_STRING("    name: ", c.lines[2].c_str());
    TEST_ASSERT_TRUE(c.has("  period_ms: 0"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_set_and_query_tree_setting);
    RUN_TEST(test_paths_are_case_insensitive_and_tolerate_spaces);
    RUN_TEST(test_errors);
    RUN_TEST(test_pins_and_enums_roundtrip);
    RUN_TEST(test_reboot_settings_say_so);
    RUN_TEST(test_speed_warning);
    RUN_TEST(test_named_settings_and_secret_masking);
    RUN_TEST(test_subtree_listing);
    RUN_TEST(test_save_writes_only_changes_and_replay_restores);
    RUN_TEST(test_replay_reports_bad_lines);
    RUN_TEST(test_restore_defaults);
    RUN_TEST(test_scene_and_servo_commands);
    RUN_TEST(test_realtime_characters);
    RUN_TEST(test_host_commands_extend_the_parser);
    RUN_TEST(test_empty_line_and_help);
    RUN_TEST(test_yaml_dump);
    return UNITY_END();
}
