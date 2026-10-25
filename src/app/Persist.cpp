#include "app/Persist.h"

#include "esp_err.h"
#include "nvs.h"

namespace ss::app::persist {

static const char* kNamespace = "servoscene";
static const char* kKey = "config";

bool load(std::string& text) {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return false;
    size_t size = 0;
    bool ok = nvs_get_blob(h, kKey, nullptr, &size) == ESP_OK;
    if (ok) {
        text.resize(size);
        ok = nvs_get_blob(h, kKey, text.data(), &size) == ESP_OK;
    }
    nvs_close(h);
    return ok;
}

static bool fail(esp_err_t e, std::string& err) {
    err = std::string("flash: ") + esp_err_to_name(e);
    return false;
}

bool save(const std::string& text, std::string& err) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(kNamespace, NVS_READWRITE, &h);
    if (e != ESP_OK) return fail(e, err);
    e = nvs_set_blob(h, kKey, text.data(), text.size());
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK || fail(e, err);
}

bool erase(std::string& err) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(kNamespace, NVS_READWRITE, &h);
    if (e != ESP_OK) return fail(e, err);
    e = nvs_erase_key(h, kKey);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK || fail(e, err);
}

}  // namespace ss::app::persist
