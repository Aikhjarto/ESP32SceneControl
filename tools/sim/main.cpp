// Desktop simulator: the firmware's command parser, settings and scene engine on the PC.
// Driven over stdin/stdout by tools/sim_server.py, which serves the web UI against it.
//
// Requests (one per line):     Replies (terminated by a line containing only ".")
//   L <command line>             parser output, ending in ok / error:N
//   R <realtime char code>       e.g. "R 63" for '?'
//   S                            status + positions, after advancing the scene clock
//   D                            saved-format dump of changed settings
//   P <file>                     replay a config file (restore)
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "core/Config.h"
#include "core/Parser.h"
#include "core/Scene.h"

using namespace ss;

static Config cfg;
static Scene scene(cfg);
static std::string savePath = "sim-config.txt";

struct StdoutSink : ReplySink {
    void line(const std::string& t) override { std::cout << t << '\n'; }
};

struct SimHost : Host {
    Parser* parser = nullptr;
    void setRunning(bool run, const char* source) override {
        if (scene.running() == run) return;
        scene.setRunning(run);
        std::cout << "[MSG:INFO: " << (run ? "Go" : "Hold") << " (" << source << ")]\n";
    }
    bool save(const std::string& text, std::string&) override {
        std::ofstream(savePath) << text;
        return true;
    }
    bool erase(std::string&) override {
        std::remove(savePath.c_str());
        return true;
    }
    void reboot() override { boot(); }
    void info(ReplySink& out) override {
        out.line("[VER:sim ESP32PWM]");
        out.line("[OPT:desktop simulator]");
        for (int i = 0; i < kMaxServos; ++i) {
            if (cfg.servo[i].pin.type == PinType::None) continue;
            out.line("[SERVO:" + std::to_string(i) + " " + Settings::formatPin(cfg.servo[i].pin) + " sim." +
                     std::to_string(i) + (cfg.servo[i].enable ? "" : " disabled") + "]");
        }
    }
    void boot() {
        setDefaults(cfg);
        std::ifstream f(savePath);
        std::stringstream text;
        text << f.rdbuf();
        StdoutSink sink;
        parser->replay(text.str(), sink);
        scene.reset();
        scene.setServoRun(-1, true);
        scene.release(-1);
        scene.setRunning(cfg.scene.autostart);
    }
};

int main(int argc, char** argv) {
    if (argc > 1) savePath = argv[1];
    SimHost host;
    Parser parser(cfg, scene, host);
    host.parser = &parser;
    host.boot();

    StdoutSink out;
    auto last = std::chrono::steady_clock::now();
    std::string req;
    while (std::getline(std::cin, req)) {
        auto now = std::chrono::steady_clock::now();
        int64_t dtUs = std::chrono::duration_cast<std::chrono::microseconds>(now - last).count();
        last = now;
        // Advance in 20 ms steps like the firmware's 50 Hz motion task.
        for (; dtUs > 0; dtUs -= 20000) scene.tick(dtUs >= 20000 ? 20000 : dtUs);

        if (req.rfind("L ", 0) == 0) {
            parser.execute(req.substr(2), out);
        } else if (req.rfind("R ", 0) == 0) {
            parser.realtime(static_cast<char>(std::stoi(req.substr(2))), out);
        } else if (req == "S") {
            out.line(scene.status());
            out.line(scene.positions());
        } else if (req == "D") {
            std::cout << parser.settings().dumpChanged();
        } else if (req.rfind("P ", 0) == 0) {
            std::ifstream f(req.substr(2));
            std::stringstream text;
            text << f.rdbuf();
            parser.replay(text.str(), out);
            out.line("ok");
        }
        std::cout << ".\n" << std::flush;
    }
}
