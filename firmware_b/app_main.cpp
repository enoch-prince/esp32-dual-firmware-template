/*
 * app_main.cpp — Firmware B
 *
 * Identical boot sequence to Firmware A but:
 *   • LED on GPIO4  (blinks at 200 ms — visually distinct from FW-A's 500 ms)
 *   • OTA manifest points to fw_b endpoint
 *   • Slot identity logged as "FirmwareB (ota_1)"
 */

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "wifi.hpp"
#include "boot_ctrl.hpp"
#include "health_monitor.hpp"
#include "net_cmd.hpp"

static const char *TAG = "fw_b";

/* ── Board config ────────────────────────────────────────────────────────── */
#define FW_B_LED_GPIO   GPIO_NUM_4   ///< different GPIO than FW-A (GPIO2)

/* ── Wi-Fi credentials  (replace before flashing) ──────────────────────── */
#define WIFI_SSID       "MINGO_CORP"
#define WIFI_PASSWORD   "MINGOblox@{20!25}?["

/* ── OTA / command server endpoints  (replace before testing OTA) ────────── */
#define OTA_MANIFEST_URL  "https://172.19.189.190:8443/fw_b/manifest.json"
// #define MQTT_BROKER_URI   "mqtts://mqtt.your-domain.com:8883"
// #define DEVICE_ID         "esp32-dev-001"

/* ── Embedded credentials (populated from firmware_b/certs/ at build time) ─ */
extern const uint8_t server_ca_pem_start[]    asm("_binary_server_ca_pem_start");
extern const uint8_t server_ca_pem_end[]      asm("_binary_server_ca_pem_end");
extern const uint8_t ecdsa_pub_pem_start[]    asm("_binary_ecdsa_pub_pem_start");
extern const uint8_t ecdsa_pub_pem_end[]      asm("_binary_ecdsa_pub_pem_end");
extern const uint8_t mqtt_ca_pem_start[]      asm("_binary_mqtt_ca_pem_start");
extern const uint8_t mqtt_ca_pem_end[]        asm("_binary_mqtt_ca_pem_end");
extern const uint8_t client_cert_pem_start[]  asm("_binary_client_cert_pem_start");
extern const uint8_t client_cert_pem_end[]    asm("_binary_client_cert_pem_end");
extern const uint8_t client_key_pem_start[]   asm("_binary_client_key_pem_start");
extern const uint8_t client_key_pem_end[]     asm("_binary_client_key_pem_end");
extern const uint8_t hmac_secret_txt_start[]  asm("_binary_hmac_secret_txt_start");
extern const uint8_t hmac_secret_txt_end[]    asm("_binary_hmac_secret_txt_end");

/* ── LED blink task ─────────────────────────────────────────────────────── */
static void led_task(void * /*arg*/)
{
    gpio_config_t io{};
    io.mode         = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = (1ULL << FW_B_LED_GPIO);
    gpio_config(&io);

    /* 200 ms blink — noticeably faster than FW-A's 500 ms */
    uint32_t tick = 0;
    while (true) {
        gpio_set_level(FW_B_LED_GPIO, 1);
        ESP_LOGI(TAG, "[tick %lu] FW-B LED ON  (slot=%.*s)",
                 tick,
                 static_cast<int>(boot_ctrl::slot_name(boot_ctrl::running_slot()).size()),
                 boot_ctrl::slot_name(boot_ctrl::running_slot()).data());
        vTaskDelay(pdMS_TO_TICKS(200));

        gpio_set_level(FW_B_LED_GPIO, 0);
        ESP_LOGI(TAG, "[tick %lu] FW-B LED OFF", tick++);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ── Entry point ────────────────────────────────────────────────────────── */
extern "C" void app_main(void)
{
    /* 1 ── NVS init ─────────────────────────────────────────────────────── */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_ret);
    }

    /* 1b ── Record this boot attempt ─────────────────────────────────────── */
    if (auto res = boot_ctrl::record_boot_attempt(); !res) {
        ESP_LOGW(TAG, "record_boot_attempt failed (err=%u)",
                 static_cast<uint8_t>(res.error()));
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Firmware B  v%s  booted", desc->version);
    ESP_LOGI(TAG, " Running slot : %.*s",
             static_cast<int>(boot_ctrl::slot_name(boot_ctrl::running_slot()).size()),
             boot_ctrl::slot_name(boot_ctrl::running_slot()).data());
    ESP_LOGI(TAG, "========================================");

    /* 2 ── LED blink ─────────────────────────────────────────────────────── */
    xTaskCreate(led_task, "led_b", 2048, nullptr, tskIDLE_PRIORITY + 1, nullptr);

    /* 3 ── Wi-Fi ─────────────────────────────────────────────────────────── */
    const wifi::Config wifi_cfg{
        .ssid                = WIFI_SSID,
        .password            = WIFI_PASSWORD,
        .connect_timeout_ms  = 15'000,
        .max_retries         = 5,
    };

    const bool wifi_ok = (wifi::connect(wifi_cfg) == ESP_OK);
    if (!wifi_ok) {
        ESP_LOGE(TAG, "Wi-Fi connect failed – running offline");
    } else {
        ESP_LOGI(TAG, "Wi-Fi connected");

        const ota_manager::Config ota_cfg{
            .manifest_url        = OTA_MANIFEST_URL,
            .ca_cert_pem         = {reinterpret_cast<const char *>(server_ca_pem_start),
                                    static_cast<size_t>(server_ca_pem_end - server_ca_pem_start - 1)},
            .ecdsa_pub_pem       = {reinterpret_cast<const char *>(ecdsa_pub_pem_start),
                                    static_cast<size_t>(ecdsa_pub_pem_end - ecdsa_pub_pem_start - 1)},
            .timeout_ms          = 10'000,
            .reboot_after_update = true,
        };

        /* 4a ── HTTP command server ─────────────────────────────────────── */
        const net_cmd::HttpConfig http_cfg{
            .port        = 8080,
            .hmac_secret = {reinterpret_cast<const char *>(hmac_secret_txt_start),
                            static_cast<size_t>(hmac_secret_txt_end - hmac_secret_txt_start)},
            .ota_cfg     = ota_cfg,
        };
        if (net_cmd::http_start(http_cfg) != ESP_OK) {
            ESP_LOGE(TAG, "HTTP server start failed");
        } else {
            ESP_LOGI(TAG, "HTTP command server listening on port %u", http_cfg.port);
        }

        /* 4b ── MQTT command subscriber ────────────────────────────────── */
        /* Disabled: see firmware_a/app_main.cpp for re-enable instructions.
        const net_cmd::MqttConfig mqtt_cfg{
            .broker_uri      = MQTT_BROKER_URI,
            .ca_cert_pem     = {reinterpret_cast<const char *>(mqtt_ca_pem_start),
                                static_cast<size_t>(mqtt_ca_pem_end - mqtt_ca_pem_start - 1)},
            .client_cert_pem = {reinterpret_cast<const char *>(client_cert_pem_start),
                                static_cast<size_t>(client_cert_pem_end - client_cert_pem_start - 1)},
            .client_key_pem  = {reinterpret_cast<const char *>(client_key_pem_start),
                                static_cast<size_t>(client_key_pem_end - client_key_pem_start - 1)},
            .device_id       = DEVICE_ID,
            .ota_cfg         = ota_cfg,
        };
        if (net_cmd::mqtt_start(mqtt_cfg) != ESP_OK) {
            ESP_LOGE(TAG, "MQTT client start failed");
        } else {
            ESP_LOGI(TAG, "MQTT client started (broker=%s)", MQTT_BROKER_URI);
        }
        */
    }

    /* 5 ── Health monitor ───────────────────────────────────────────────── */
    health_monitor::register_probe({
        .name   = "wifi_sta",
        .probe  = [] {
            wifi_ap_record_t ap{};
            return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
        },
        .weight = 1,
    });

    health_monitor::start({
        .check_interval_ms      = 10'000,
        .failure_threshold      = 3,
        .auto_switch_on_failure = true,
    });

    /* 6 ── Mark healthy ─────────────────────────────────────────────────── */
    if (auto res = boot_ctrl::mark_healthy(); !res) {
        ESP_LOGW(TAG, "mark_healthy failed (err=%u)",
                 static_cast<uint8_t>(res.error()));
    } else {
        ESP_LOGI(TAG, "Boot marked healthy");
    }

    ESP_LOGI(TAG, "Firmware B startup complete – LED blinking on GPIO%d", FW_B_LED_GPIO);
}
