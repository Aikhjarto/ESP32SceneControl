#include "app/MotionTask.h"

#include <algorithm>
#include <atomic>

#include "app/App.h"
#include "core/Motion.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace ss::app {

static const char* TAG = "motion";

static TaskHandle_t gTask;
static int64_t gTickUs = 20000;
static int64_t gEnableAtUs[kMaxServos];  // staggered power-up
static bool gOn[kMaxServos];
static float gPulse[kMaxServos];
static bool gWant[kMaxServos];
static int8_t gOutLevel[kMaxOutputs];  // level to drive: 1 high, 0 low

// Button state
static bool gButtonUsed;
static int gButtonGpio;
static bool gButtonActiveLow;
static bool gButtonStable;
static int64_t gButtonChangeUs;

static std::atomic<uint32_t> gRunGeneration;

uint32_t runGeneration() { return gRunGeneration; }

void setRunning(bool run, const char* source) {
    bool changed;
    {
        Locked l;
        changed = scene().running() != run;
        scene().setRunning(run);
        ++gRunGeneration;  // every request counts, also one that changes nothing (e.g. Stop while held)
    }
    if (changed) broadcast(std::string("[MSG:INFO: ") + (run ? "Go" : "Hold") + " (" + source + ")]");
}

static void initButton(const ButtonConfig& b) {
    gButtonUsed = b.pin.type == PinType::Gpio && GPIO_IS_VALID_GPIO(b.pin.num);
    if (!gButtonUsed) return;
    gButtonGpio = b.pin.num;
    gButtonActiveLow = b.activeLow;
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << gButtonGpio;
    io.mode = GPIO_MODE_INPUT;
    // GPIO 34..39 have no internal pulls; those need an external resistor.
    io.pull_up_en = b.pullup && b.activeLow ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    io.pull_down_en = b.pullup && !b.activeLow ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGW(TAG, "button gpio.%d unusable", gButtonGpio);
        gButtonUsed = false;
        return;
    }
    gButtonStable = (gpio_get_level(static_cast<gpio_num_t>(gButtonGpio)) == 0) == gButtonActiveLow;
}

// Debounced press detection: the level must stay unchanged for debounceMs.
static bool buttonPressed(int64_t nowUs, uint32_t debounceMs) {
    if (!gButtonUsed) return false;
    bool level = (gpio_get_level(static_cast<gpio_num_t>(gButtonGpio)) == 0) == gButtonActiveLow;
    if (level == gButtonStable) {
        gButtonChangeUs = nowUs;
        return false;
    }
    if (nowUs - gButtonChangeUs < static_cast<int64_t>(debounceMs) * 1000) return false;
    gButtonStable = level;
    return level;
}

static void onTimer(void*) { xTaskNotifyGive(gTask); }

static void motionTask(void*) {
    int64_t last = esp_timer_get_time();
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const int64_t now = esp_timer_get_time();
        // After a stall (e.g. flash writes) the scene clock lags instead of leaping, so
        // servos never jump to catch up.
        const int64_t dt = std::min<int64_t>(now - last, 4 * gTickUs);
        last = now;

        uint32_t debounceMs;
        {
            Locked l;
            debounceMs = config().button.debounceMs;
        }
        if (buttonPressed(now, debounceMs)) setRunning(!scene().running(), "button");

        {
            Locked l;
            Scene& sc = scene();
            sc.tick(dt);
            for (int i = 0; i < kMaxServos; ++i) {
                gWant[i] = sc.hasOutput(i) && now >= gEnableAtUs[i];
                if (gWant[i]) gPulse[i] = pulseUs(config().servo[i], sc.output(i));
            }
            for (int o = 0; o < kMaxOutputs; ++o) {
                // Inactive (disabled) outputs go to their "off" level.
                const bool on = sc.hasOutputState(o) && sc.outputOn(o);
                gOutLevel[o] = outputs().levelFor(o, on, config().out[o]);
            }
        }

        Outputs& out = outputs();
        for (int i = 0; i < kMaxServos; ++i) {
            if (out.channel(i).kind == HwKind::None) continue;
            if (gWant[i]) {
                out.setPulseUs(i, gPulse[i]);
                gOn[i] = true;
            } else if (gOn[i]) {
                out.off(i);
                gOn[i] = false;
            }
        }
        for (int o = 0; o < kMaxOutputs; ++o) out.setDigital(o, gOutLevel[o]);  // only changes are written
        out.flush();
    }
}

void startMotion() {
    uint32_t hz;
    {
        Locked l;
        const Config& cfg = config();
        hz = std::clamp<uint32_t>(cfg.scene.updateHz, 10, 200);
        gTickUs = 1000000 / hz;
        initButton(cfg.button);
        // Power servos up one after another so they don't all jump at once.
        int64_t at = esp_timer_get_time();
        for (int i = 0; i < kMaxServos; ++i) {
            gEnableAtUs[i] = at;
            if (servoActive(cfg.servo[i]) && outputs().channel(i).kind != HwKind::None) {
                at += static_cast<int64_t>(cfg.scene.staggerMs) * 1000;
            }
        }
        scene().setRunning(cfg.scene.autostart);
    }

    xTaskCreatePinnedToCore(motionTask, "motion", 4096, nullptr, configMAX_PRIORITIES - 3, &gTask, 1);

    esp_timer_create_args_t ta = {};
    ta.callback = onTimer;
    ta.name = "motion";
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&ta, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, gTickUs));
    ESP_LOGI(TAG, "running at %lu Hz", static_cast<unsigned long>(hz));
}

}  // namespace ss::app
