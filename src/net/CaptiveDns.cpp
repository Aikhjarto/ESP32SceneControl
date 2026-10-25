#include "net/CaptiveDns.h"

#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

namespace ss::net {

static const char* TAG = "dns";

static uint32_t gIp;  // network byte order

// Skips the question name; returns the offset after it, or 0 if malformed.
static size_t skipName(const uint8_t* buf, size_t len, size_t pos) {
    while (pos < len) {
        uint8_t l = buf[pos];
        if (l == 0) return pos + 1;
        if ((l & 0xC0) == 0xC0) return pos + 2;
        pos += l + 1;
    }
    return 0;
}

static void dnsTask(void*) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (sock < 0 || bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "cannot bind port 53");
        if (sock >= 0) close(sock);
        vTaskDelete(nullptr);
        return;
    }

    uint8_t buf[512];
    for (;;) {
        sockaddr_in from = {};
        socklen_t fromLen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n < 12) continue;
        const uint16_t qdcount = (buf[4] << 8) | buf[5];
        if ((buf[2] & 0x80) || qdcount != 1) continue;  // only plain queries with one question

        size_t qEnd = skipName(buf, n, 12);
        if (qEnd == 0 || qEnd + 4 > static_cast<size_t>(n)) continue;
        const uint16_t qtype = (buf[qEnd] << 8) | buf[qEnd + 1];
        size_t len = qEnd + 4;  // header + question, additional records dropped

        buf[2] = 0x84 | (buf[2] & 0x01);  // response, authoritative, keep RD
        buf[3] = 0x80;                      // recursion available, no error
        buf[6] = buf[7] = 0;                // ANCOUNT
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        if (qtype == 1) {  // A record → our address
            buf[7] = 1;
            const uint8_t answer[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
            std::memcpy(buf + len, answer, sizeof(answer));
            len += sizeof(answer);
            std::memcpy(buf + len, &gIp, 4);
            len += 4;
        }
        sendto(sock, buf, len, 0, reinterpret_cast<sockaddr*>(&from), fromLen);
    }
}

void startCaptiveDns(const std::string& ip) {
    gIp = inet_addr(ip.c_str());
    xTaskCreatePinnedToCore(dnsTask, "dns", 3072, nullptr, 3, nullptr, 0);
}

}  // namespace ss::net
