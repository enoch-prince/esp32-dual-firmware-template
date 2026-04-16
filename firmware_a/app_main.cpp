/*
 * app_main.cpp — Firmware A
 *
 * Boot sequence:
 *   1. NVS init  →  record_boot_attempt
 *   2. LED blink task (GPIO2, 500 ms)
 *   3. Wi-Fi connect
 *   4. HTTP command server  +  MQTT command subscriber
 *   5. Health monitor (Wi-Fi probe, 10 s interval)
 *   6. mark_healthy
 * 
 *  TODO:
 *  - Add WiFi Manager to configure Wi-Fi credentials via captive portal if connection fails
 *  - Fix app crashing if Wi-Fi connection fails (currently happens because HTTP server startup doesn't handle Wi-Fi failure gracefully)
 *  
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

static const char *TAG = "fw_a";

/* ── Board config ────────────────────────────────────────────────────────── */
#define FW_A_LED_GPIO   GPIO_NUM_2   ///< onboard LED on most ESP32 DevKit boards

/* ── Wi-Fi credentials  (replace before flashing) ──────────────────────── */
#define WIFI_SSID       "MINGO_CORP"
#define WIFI_PASSWORD   "MINGOblox@{20!25}?["

/* ── OTA / command server endpoints  (replace before testing OTA) ────────── */
// #define OTA_MANIFEST_URL  "https://172.19.189.190:8443/fw_a/manifest.json"  // WSL2
#define OTA_MANIFEST_URL  "https://192.168.0.105:8443/fw_a/manifest.json"  // Windows
// #define MQTT_BROKER_URI   "mqtts://mqtt.your-domain.com:8883"
// #define DEVICE_ID         "esp32-dev-001"

/* ── Embedded credentials (populated from firmware_a/certs/ at build time) ─ */
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
    io.pin_bit_mask = (1ULL << FW_A_LED_GPIO);
    gpio_config(&io);

    uint32_t tick = 0;
    while (true) {
        gpio_set_level(FW_A_LED_GPIO, 1);
        ESP_LOGI(TAG, "[tick %lu] FW-A LED ON  (slot=%.*s)",
                 tick,
                 static_cast<int>(boot_ctrl::slot_name(boot_ctrl::running_slot()).size()),
                 boot_ctrl::slot_name(boot_ctrl::running_slot()).data());
        vTaskDelay(pdMS_TO_TICKS(500));

        gpio_set_level(FW_A_LED_GPIO, 0);
        ESP_LOGI(TAG, "[tick %lu] FW-A LED OFF", tick++);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ── Entry point ────────────────────────────────────────────────────────── */
extern "C" void app_main(void)
{
    /* 0 ── Reset all board GPIOs that either firmware slot may have left
     *       active.  esp_restart() (software reset) does NOT clear GPIO
     *       output registers on ESP32, so the previous firmware's LED can
     *       stay driven unless we reclaim the pin before use.
     *       gpio_reset_pin() returns each pad to its reset state:
     *       input, no pull, GPIO-matrix disconnected.               ────── */
    // gpio_reset_pin(GPIO_NUM_2);   // FW-A LED
    // gpio_reset_pin(GPIO_NUM_4);   // FW-B LED

    /* 1 ── NVS init (must come before boot_ctrl which reads/writes NVS) ─── */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_ret);
    }

    /* 1b ── Record this boot attempt; auto-rollback if we keep crashing ── */
    if (auto res = boot_ctrl::record_boot_attempt(); !res) {
        ESP_LOGW(TAG, "record_boot_attempt failed (err=%u)",
                 static_cast<uint8_t>(res.error()));
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Firmware A  v%s  booted", desc->version);
    ESP_LOGI(TAG, " Running slot : %.*s",
             static_cast<int>(boot_ctrl::slot_name(boot_ctrl::running_slot()).size()),
             boot_ctrl::slot_name(boot_ctrl::running_slot()).data());
    ESP_LOGI(TAG, "========================================");

    /* 2 ── LED blink (visual indicator that FW-A is running) ────────────── */
    xTaskCreate(led_task, "led_a", 2048, nullptr, tskIDLE_PRIORITY + 1, nullptr);

    /* 3 ── Wi-Fi ─────────────────────────────────────────────────────────── */
    const wifi::Config wifi_cfg{
        .ssid                = WIFI_SSID,
        .password            = WIFI_PASSWORD,
        .connect_timeout_ms  = 15'000,
        .max_retries         = 5,
    };

    const bool wifi_ok = (wifi::connect(wifi_cfg) == ESP_OK);
    if (!wifi_ok) {
        ESP_LOGE(TAG, "Wi-Fi connect failed – running offline (no OTA/commands)");
    } else {
        ESP_LOGI(TAG, "Wi-Fi connected");

        /* Build OTA config once; reused by both HTTP and MQTT transports */
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
                            static_cast<size_t>(hmac_secret_txt_end - hmac_secret_txt_start - 1)},
            .ota_cfg     = ota_cfg,
        };
        if (net_cmd::http_start(http_cfg) != ESP_OK) {
            ESP_LOGE(TAG, "HTTP server start failed");
        } else {
            ESP_LOGI(TAG, "HTTP command server listening on port %u", http_cfg.port);
        }

        /* 4b ── MQTT command subscriber ────────────────────────────────── */
        /* Disabled: espressif/esp-mqtt not yet installed in this environment.
         * Enable by running: idf.py add-dependency "espressif/esp-mqtt>=2.0.0"
         * then un-comment mqtt_cmd.cpp in components/net_cmd/CMakeLists.txt.
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

    ESP_LOGI(TAG, "Firmware A startup complete – LED blinking on GPIO%d", FW_A_LED_GPIO);
    /* app_main returns; LED task + health monitor continue on their own tasks */

    esp_register_shutdown_handler([]() {
        // Runs just before esp_restart(), from any call site
        gpio_set_level(FW_A_LED_GPIO, 0);
        gpio_reset_pin(FW_A_LED_GPIO);
        // stop PWM, flush UART TX, etc.
    });

}
