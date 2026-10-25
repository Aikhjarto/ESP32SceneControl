#include "net/Ota.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>

#include "app/App.h"
#include "app/MotionTask.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/HttpRecv.h"
#include "net/SignedImage.h"
#include "net/Wifi.h"

extern const uint8_t ota_verify_key_start[] asm("_binary_ota_verify_key_bin_start");

namespace ss::net {

static const char* TAG = "ota";

constexpr size_t kChunk = 4096;
constexpr size_t kHeaderBytes = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
constexpr int64_t kConfirmAfterUs = 10'000'000;
constexpr int kUpdatePort = 9999;

static std::atomic<bool> gBusy{false};

// ---------------------------------------------------------------------------
// Image writer shared by upload and download

class OtaWriter {
public:
    ~OtaWriter() {
        if (handle_) esp_ota_abort(handle_);
        // A rejected or partial image must never be bootable: the bootloader falls back to
        // other slots without checking signatures, so wipe the image header.
        if (started_ && !installed_) esp_partition_erase_range(target_, 0, target_->erase_size);
    }

    bool begin(size_t total, std::string& err) {
        target_ = esp_ota_get_next_update_partition(nullptr);
        if (!target_) return fail("no OTA partition", err);
        if (total < kHeaderBytes + SignedImage::kSigBlock) return fail("file too small to be a signed firmware image", err);
        if (total > target_->size) return fail("image does not fit the OTA slot", err);
        total_ = total;
        return signature_.begin(total, err);
    }

    // The first call must contain at least kHeaderBytes, so the image can be checked
    // before anything is erased.
    bool write(const uint8_t* data, size_t len, std::string& err) {
        if (written_ + len > total_) return fail("more data than announced", err);
        if (written_ == 0) {
            if (len < kHeaderBytes || !checkImage(data, err)) return err.empty() ? fail("short read", err) : false;
            esp_err_t e = esp_ota_begin(target_, OTA_WITH_SEQUENTIAL_WRITES, &handle_);
            if (e != ESP_OK) {
                handle_ = 0;
                return fail(std::string("esp_ota_begin: ") + esp_err_to_name(e), err);
            }
            started_ = true;
            ESP_LOGI(TAG, "writing %s to %s (%u bytes)", version_.c_str(), target_->label, unsigned(total_));
        }
        signature_.feed(data, len);
        esp_err_t e = esp_ota_write(handle_, data, len);
        if (e != ESP_OK) return fail(std::string("esp_ota_write: ") + esp_err_to_name(e), err);
        written_ += len;
        return true;
    }

    bool finish(std::string& err) {
        if (written_ != total_) return fail("incomplete image", err);
        // Checked before the new slot can become the boot partition.
        if (!signature_.verify(ota_verify_key_start, err)) return false;
        esp_err_t e = esp_ota_end(handle_);  // also checks the image's own checksum
        handle_ = 0;
        if (e == ESP_OK) e = esp_ota_set_boot_partition(target_);
        if (e != ESP_OK) return fail(e == ESP_ERR_OTA_VALIDATE_FAILED ? "image is corrupt" : esp_err_to_name(e), err);
        installed_ = true;
        return true;
    }

    const std::string& version() const { return version_; }
    size_t written() const { return written_; }

private:
    static bool fail(const std::string& what, std::string& err) {
        err = what;
        return false;
    }

    // Rejects anything that is not firmware for this chip and this project.
    bool checkImage(const uint8_t* buf, std::string& err) {
        auto* hdr = reinterpret_cast<const esp_image_header_t*>(buf);
        auto* desc = reinterpret_cast<const esp_app_desc_t*>(buf + sizeof(esp_image_header_t) +
                                                             sizeof(esp_image_segment_header_t));
        if (hdr->magic != ESP_IMAGE_HEADER_MAGIC || desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
            return fail("not an ESP32 firmware image (use firmware-signed.bin)", err);
        }
        if (hdr->chip_id != ESP_CHIP_ID_ESP32) return fail("firmware was built for a different chip", err);
        const esp_app_desc_t* running = esp_app_get_description();
        if (std::strncmp(desc->project_name, running->project_name, sizeof(desc->project_name)) != 0) {
            return fail(std::string("firmware belongs to project \"") +
                            std::string(desc->project_name, strnlen(desc->project_name, sizeof(desc->project_name))) +
                            "\", not \"" + running->project_name + "\"",
                        err);
        }
        version_ = std::string(desc->version, strnlen(desc->version, sizeof(desc->version)));
        return true;
    }

    const esp_partition_t* target_ = nullptr;
    esp_ota_handle_t handle_ = 0;
    bool started_ = false;    // flash of the target slot has been touched
    bool installed_ = false;  // verified and selected as boot partition
    SignedImage signature_;
    size_t total_ = 0;
    size_t written_ = 0;
    std::string version_;
};

// Holds the scene while flash is written; on failure the scene resumes.
class Session {
public:
    bool start(std::string& err) {
        if (gBusy.exchange(true)) {
            err = "an update is already running";
            return false;
        }
        active_ = true;
        app::Locked l;
        wasRunning_ = app::scene().running();
        return true;
    }

    void hold() { app::setRunning(false, "firmware update"); }

    void end(bool ok, const std::string& version, const std::string& err) {
        if (ok) {
            app::broadcast("[MSG:INFO: Firmware " + version + " installed, restarting]");
            app::restartSoon();
        } else {
            ESP_LOGW(TAG, "update failed: %s", err.c_str());
            app::broadcast("[MSG:ERR: Firmware update failed: " + err + "]");
            if (wasRunning_) app::setRunning(true, "firmware update failed");
        }
        gBusy = false;
        active_ = false;
    }

    ~Session() {
        if (active_) gBusy = false;
    }

private:
    bool active_ = false;
    bool wasRunning_ = false;
};

// ---------------------------------------------------------------------------
// Push: POST /update

static esp_err_t reply(httpd_req_t* req, const char* status, const std::string& text) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, text.data(), text.size());
}

esp_err_t otaUploadHandler(httpd_req_t* req) {
    Session session;
    std::string err;
    if (!session.start(err)) return reply(req, "409 Conflict", err);

    OtaWriter writer;
    bool ok = writer.begin(req->content_len, err);
    if (ok) session.hold();
    std::unique_ptr<uint8_t[]> buf(new uint8_t[kChunk]);
    size_t received = 0;
    while (ok && received < req->content_len) {
        size_t want = std::min(kChunk, req->content_len - received);
        if (!recvExact(req, reinterpret_cast<char*>(buf.get()), want)) {
            err = "upload interrupted or stalled";
            ok = false;
            break;
        }
        ok = writer.write(buf.get(), want, err);
        received += want;
    }
    if (ok) ok = writer.finish(err);

    session.end(ok, writer.version(), err);
    if (!ok) return reply(req, "400 Bad Request", err);
    return reply(req, "200 OK", "Firmware " + writer.version() + " installed, restarting");
}

// ---------------------------------------------------------------------------
// Pull: download from a URL

static bool download(const std::string& url, OtaWriter& writer, Session& session, std::string& err) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        err = "invalid URL";
        return false;
    }
    struct Cleanup {
        esp_http_client_handle_t c;
        ~Cleanup() {
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
        }
    } cleanup{client};

    if (esp_http_client_open(client, 0) != ESP_OK) {
        err = "cannot connect to " + url + " (is tools/serve_firmware.py running and port 9999 open?)";
        return false;
    }
    int64_t length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        err = "server answered HTTP " + std::to_string(status);
        return false;
    }
    if (length <= 0) {
        err = "server did not send a Content-Length";
        return false;
    }
    if (!writer.begin(static_cast<size_t>(length), err)) return false;
    session.hold();

    std::unique_ptr<uint8_t[]> buf(new uint8_t[kChunk]);
    int lastPct = -1;
    while (writer.written() < static_cast<size_t>(length)) {
        size_t want = std::min<size_t>(kChunk, length - writer.written());
        size_t have = 0;
        while (have < want) {
            int r = esp_http_client_read(client, reinterpret_cast<char*>(buf.get()) + have, want - have);
            if (r <= 0) break;
            have += r;
        }
        if (have < want) {
            err = "download interrupted";
            return false;
        }
        if (!writer.write(buf.get(), have, err)) return false;
        int pct = static_cast<int>(writer.written() * 100 / length);
        if (pct / 10 != lastPct / 10) {
            app::broadcast("[OTA:download " + std::to_string(pct) + "%]");
            lastPct = pct;
        }
    }
    return writer.finish(err);
}

static void downloadTask(void* arg) {
    std::unique_ptr<std::string> url(static_cast<std::string*>(arg));
    {
        Session session;
        std::string err;
        if (session.start(err)) {
            OtaWriter writer;
            bool ok = download(*url, writer, session, err);
            session.end(ok, writer.version(), err);
        } else {
            app::broadcast("[MSG:ERR: " + err + "]");
        }
    }
    vTaskDelete(nullptr);
}

bool startOtaDownload(const std::string& url, std::string& err) {
    if (gBusy) {
        err = "an update is already running";
        return false;
    }
    if (url.rfind("http://", 0) != 0) {
        // No CA certificates are built in, so https would always fail later.
        err = "URL must start with http:// (https is not supported)";
        return false;
    }
    auto* arg = new std::string(url);
    if (xTaskCreatePinnedToCore(downloadTask, "ota", 8192, arg, 4, nullptr, 0) != pdPASS) {
        delete arg;
        err = "out of memory";
        return false;
    }
    return true;
}

std::string defaultUpdateUrl() {
    const std::string ip = firstClientIp();
    if (ip.empty()) return "";
    return "http://" + ip + ":" + std::to_string(kUpdatePort) + "/firmware.bin";
}

// ---------------------------------------------------------------------------
// Rollback protection and info

static void confirm(void*) {
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "new firmware confirmed, rollback cancelled");
    }
}

void confirmFirmwareLater() {
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    ESP_LOGW(TAG, "running a new firmware; it is confirmed after %lld s without a reset", kConfirmAfterUs / 1000000);
    esp_timer_create_args_t ta = {};
    ta.callback = confirm;
    ta.name = "ota-confirm";
    esp_timer_handle_t t;
    if (esp_timer_create(&ta, &t) == ESP_OK) esp_timer_start_once(t, kConfirmAfterUs);
}

std::string firmwareInfo() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    std::string out = std::string("[MSG:INFO: Firmware ") + esp_app_get_description()->version + " in " +
                      running->label;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        out += " (not yet confirmed)";
    }
    const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
    esp_app_desc_t desc;
    if (other && esp_ota_get_partition_description(other, &desc) == ESP_OK) {
        out += std::string(", ") + other->label + " holds " + desc.version;
    }
    return out + "]";
}

}  // namespace ss::net
