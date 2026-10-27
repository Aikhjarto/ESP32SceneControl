#include "net/Wifi.h"

#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "dhcpserver/dhcpserver.h"
#include "lwip/ip4_addr.h"

namespace ss::net {

static const char* TAG = "wifi";

static std::string gSsid;
static std::string gIp;

static uint32_t gFirstClient;  // network byte order, 0 = none yet

const std::string& apSsid() { return gSsid; }
const std::string& apIp() { return gIp; }

std::string firstClientIp() {
    if (!gFirstClient) return "";
    esp_ip4_addr_t a = {gFirstClient};
    char buf[16];
    esp_ip4addr_ntoa(&a, buf, sizeof(buf));
    return buf;
}

static void onIpAssigned(void*, esp_event_base_t, int32_t, void* data) {
    auto* e = static_cast<ip_event_assigned_ip_to_client_t*>(data);
    if (!gFirstClient) gFirstClient = e->ip.addr;
    ESP_LOGI(TAG, "client got " IPSTR, IP2STR(&e->ip));
}

static void onWifiEvent(void*, esp_event_base_t, int32_t id, void* data) {
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        auto* e = static_cast<wifi_event_ap_staconnected_t*>(data);
        ESP_LOGI(TAG, "client " MACSTR " joined", MAC2STR(e->mac));
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        auto* e = static_cast<wifi_event_ap_stadisconnected_t*>(data);
        ESP_LOGI(TAG, "client " MACSTR " left", MAC2STR(e->mac));
    }
}

void startAccessPoint(const NetConfig& cfg) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t* netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onWifiEvent, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, onIpAssigned, nullptr));

    if (cfg.ssid[0]) {
        gSsid = cfg.ssid;
    } else {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        char buf[24];
        snprintf(buf, sizeof(buf), "SceneControl-%02X%02X", mac[4], mac[5]);
        gSsid = buf;
    }

    // Static AP address; the ESP is also gateway and (for the captive portal) DNS server.
    esp_netif_ip_info_t ip = {};
    if (!ip4addr_aton(cfg.ip, reinterpret_cast<ip4_addr_t*>(&ip.ip))) IP4_ADDR(&ip.ip, 192, 168, 4, 1);
    ip.gw = ip.ip;
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(netif);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip));
    if (cfg.captive) {
        esp_netif_dns_info_t dns = {};
        dns.ip.u_addr.ip4.addr = ip.ip.addr;
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dhcps_offer_t offerDns = OFFER_DNS;
        esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offerDns, sizeof(offerDns));
        esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    }
    ESP_ERROR_CHECK(esp_netif_dhcps_start(netif));
    if (cfg.hostname[0]) esp_netif_set_hostname(netif, cfg.hostname);

    char ipStr[16];
    esp_ip4addr_ntoa(&ip.ip, ipStr, sizeof(ipStr));
    gIp = ipStr;

    wifi_config_t wc = {};
    std::strncpy(reinterpret_cast<char*>(wc.ap.ssid), gSsid.c_str(), sizeof(wc.ap.ssid));
    wc.ap.ssid_len = static_cast<uint8_t>(gSsid.size());
    std::strncpy(reinterpret_cast<char*>(wc.ap.password), cfg.password, sizeof(wc.ap.password));
    wc.ap.channel = static_cast<uint8_t>(cfg.channel);
    wc.ap.max_connection = 4;
    wc.ap.authmode = cfg.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "access point \"%s\" on channel %lu, http://%s/", gSsid.c_str(),
             static_cast<unsigned long>(cfg.channel), gIp.c_str());
}

}  // namespace ss::net
