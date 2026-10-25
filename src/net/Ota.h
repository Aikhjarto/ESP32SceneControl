// Over-the-air firmware update, either pushed (POST /update from the browser) or pulled
// (the ESP downloads http://<laptop>:9999/firmware.bin). Images must carry a valid
// ECDSA P-256 signature (espsecure "version 1" block) for the key in keys/ota_verify_key.bin;
// serial flashing is unaffected. A new image must run for a while before it is confirmed;
// if it resets earlier, the bootloader rolls back to the previous firmware.
#pragma once

#include <string>

#include "esp_http_server.h"

namespace ss::net {

esp_err_t otaUploadHandler(httpd_req_t* req);

// Starts a background download; progress ([OTA:...]) and the result are broadcast.
bool startOtaDownload(const std::string& url, std::string& err);

// http://<first client that joined the AP>:9999/firmware.bin, or empty if none joined yet.
std::string defaultUpdateUrl();

// Call once startup succeeded: confirms a freshly updated image after a grace period.
void confirmFirmwareLater();

// "[MSG:INFO: Firmware 0.1.0 in ota_0 ...]" for $I
std::string firmwareInfo();

}  // namespace ss::net
