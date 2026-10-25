#include "app/App.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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

// Broadcasts are delivered by a low-priority task: writing to the UART or queueing
// WebSocket frames can block, and callers include the 50 Hz motion task.
static QueueHandle_t gBroadcastQueue;

static void broadcastTask(void*) {
    for (;;) {
        std::string* text;
        xQueueReceive(gBroadcastQueue, &text, portMAX_DELAY);
        gBroadcaster(*text);
        delete text;
    }
}

void setBroadcaster(Broadcaster b) {
    gBroadcaster = b;
    if (!gBroadcastQueue) {
        gBroadcastQueue = xQueueCreate(16, sizeof(std::string*));
        xTaskCreatePinnedToCore(broadcastTask, "broadcast", 4096, nullptr, 4, nullptr, 0);
    }
}

void broadcast(const std::string& text) {
    if (!gBroadcastQueue) {
        ESP_LOGI(TAG, "%s", text.c_str());
        return;
    }
    auto* copy = new std::string(text);
    if (xQueueSend(gBroadcastQueue, &copy, 0) != pdTRUE) {  // never wait
        delete copy;
        ESP_LOGW(TAG, "broadcast queue full, dropped: %s", text.c_str());
    }
}

}  // namespace ss::app
