/*
 * wifi.cpp
 *
 * Station-mode Wi-Fi with event-driven connection management.
 * Uses C++17 designated initialisers for ESP-IDF config structs.
 */

#include "wifi.hpp"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <cstring>

static const char *TAG = "wifi";

namespace {

constexpr EventBits_t kConnected = BIT0;
constexpr EventBits_t kFailed    = BIT1;

EventGroupHandle_t g_event_group{nullptr};
uint8_t            g_retry_count{0};
uint8_t            g_max_retries{5};

bool           s_base_initialized{false};  // esp_netif_init + event loop: once per power cycle
bool           s_wifi_initialized{false};  // esp_wifi_init: reset by disconnect()
esp_netif_t   *s_sta_netif{nullptr};

void event_handler(void            *arg,
                   esp_event_base_t base,
                   int32_t          event_id,
                   void            *event_data)
{
    if (base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            if (g_retry_count < g_max_retries) {
                ++g_retry_count;
                ESP_LOGW(TAG, "Reconnecting... (%u/%u)",
                         g_retry_count, g_max_retries);
                esp_wifi_connect();
            } else {
                xEventGroupSetBits(g_event_group, kFailed);
            }
            break;

        default:
            break;
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const auto *ev = static_cast<ip_event_got_ip_t *>(event_data);
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        g_retry_count = 0;
        xEventGroupSetBits(g_event_group, kConnected);
    }
}

} // anonymous namespace

namespace wifi {

esp_err_t connect(const Config &cfg)
{
    g_max_retries = cfg.max_retries;
    g_retry_count = 0;

    if (!s_base_initialized) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        s_base_initialized = true;
    }

    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }

    if (!s_wifi_initialized) {
        wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
        s_wifi_initialized = true;
    }

    esp_wifi_stop(); // safe no-op if not yet started; stops cleanly on retry

    g_event_group = xEventGroupCreate();

    esp_event_handler_instance_t inst_wifi, inst_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, nullptr, &inst_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, nullptr, &inst_ip));

    wifi_config_t wifi_cfg{};
    std::strncpy(reinterpret_cast<char *>(wifi_cfg.sta.ssid),
                 cfg.ssid.data(),
                 sizeof(wifi_cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char *>(wifi_cfg.sta.password),
                 cfg.password.data(),
                 sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: %.*s",
             static_cast<int>(cfg.ssid.size()), cfg.ssid.data());

    const TickType_t timeout = pdMS_TO_TICKS(cfg.connect_timeout_ms);
    const EventBits_t bits   = xEventGroupWaitBits(
        g_event_group, kConnected | kFailed, pdFALSE, pdFALSE, timeout);

    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,    inst_wifi);
    esp_event_handler_instance_unregister(IP_EVENT,   IP_EVENT_STA_GOT_IP, inst_ip);
    vEventGroupDelete(g_event_group);
    g_event_group = nullptr;

    if (bits & kConnected) return ESP_OK;
    if (bits & kFailed)    return ESP_FAIL;
    return ESP_ERR_TIMEOUT;
}

void disconnect()
{
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    s_wifi_initialized = false;
}

} // namespace wifi
