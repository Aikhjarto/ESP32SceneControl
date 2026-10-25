#include "app/App.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace ss::app {

static const char* TAG = "app";

static Config gConfig;
static Scene gScene(gConfig);
static Outputs gOutputs;
static SemaphoreHandle_t gLock;
static Broadcaster gBroadcaster;
static std::string gBootLog;

Config& config() { return gConfig; }
Scene& scene() { return gScene; }
Outputs& outputs() { return gOutputs; }
std::string& bootLog() { return gBootLog; }

void initLock() { gLock = xSemaphoreCreateRecursiveMutex(); }
void lock() { xSemaphoreTakeRecursive(gLock, portMAX_DELAY); }
void unlock() { xSemaphoreGiveRecursive(gLock); }

void restartSoon() {
    esp_timer_create_args_t ta = {};
    ta.callback = [](void*) { esp_restart(); };
    ta.name = "restart";
    esp_timer_handle_t t;
    if (esp_timer_create(&ta, &t) == ESP_OK) esp_timer_start_once(t, 500000);
}

void setBroadcaster(Broadcaster b) { gBroadcaster = b; }

void broadcast(const std::string& text) {
    if (gBroadcaster) {
        gBroadcaster(text);
    } else {
        ESP_LOGI(TAG, "%s", text.c_str());
    }
}

}  // namespace ss::app
