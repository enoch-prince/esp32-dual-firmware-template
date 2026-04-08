// /*
// mqtt_cmd.cpp
// MQTT subscriber for remote firmware switch and OTA update commands.
// Uses mutual TLS; the broker authenticates each device by client certificate.

// C++17 Compatible - No std::format, uses ETL strings, expected
// */
// #include "net_cmd.hpp"
// #include <cstring>
// #include <cstdio>
// #include "esp_log.h"
// #include "esp_app_desc.h"
// #include "mqtt_client.h"
// #include "cJSON.h"
// #include "boot_ctrl.hpp"
// #include "ota_manager.hpp"
// #include "etl/string.h"

// static const char *TAG = "mqtt_cmd";

// namespace {
//     net_cmd::MqttConfig     g_cfg;
//     esp_mqtt_client_handle_t g_client{nullptr};

//     /* C++17: Use ETL fixed strings instead of std::string to avoid heap alloc */
//     etl::string_fixed<96> g_topic_switch;
//     etl::string_fixed<96> g_topic_update;
//     etl::string_fixed<96> g_topic_status_cmd;
//     etl::string_fixed<96> g_topic_status_pub;

//     /* ── Helper: Build topic string without std::format ─────────────────── */
//     void build_topic(etl::string_fixed<96>& out, 
//                      std::string_view prefix,
//                      std::string_view device_id,
//                      std::string_view suffix) noexcept
//     {
//         out.clear();
//         out.reserve(96);
//         out.append(prefix.data(), prefix.size());
//         out.append(device_id.data(), device_id.size());
//         out.append(suffix.data(), suffix.size());
//     }

//     /* ── Publish device status ───────────────────────────────────────────────── */
//     void publish_status()
//     {
//         if (!g_client) return;
//         const esp_app_desc_t *desc = esp_app_get_description();

//         cJSON *root = cJSON_CreateObject();
//         cJSON_AddStringToObject(root, "running_slot",
//             boot_ctrl::slot_name(boot_ctrl::running_slot()).data());
//         cJSON_AddStringToObject(root, "version", desc->version);

//         char *json_str = cJSON_PrintUnformatted(root);
//         cJSON_Delete(root);

//         esp_mqtt_client_publish(g_client, 
//                                 g_topic_status_pub.c_str(),
//                                 json_str, 
//                                 0, 
//                                 1,  // qos
//                                 0); // retain
//         free(json_str);
//     }

//     /* ── Decode slot from payload ────────────────────────────────────────────── */
//     [[nodiscard]] boot_ctrl::Slot slot_from_payload(const char *data, int len) noexcept
//     {
//         return (len > 0 && (data[0] == 'B' || data[0] == 'b'))
//             ? boot_ctrl::Slot::FirmwareB
//             : boot_ctrl::Slot::FirmwareA;
//     }

//     /* ── MQTT event handler ──────────────────────────────────────────────────── */
//     void mqtt_event_handler(void            *arg,
//                             esp_event_base_t base,
//                             int32_t          event_id,
//                             void            *event_data)
//     {
//         auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);
//         switch (event_id) {
//             case MQTT_EVENT_CONNECTED:
//                 ESP_LOGI(TAG, "MQTT connected");
//                 esp_mqtt_client_subscribe(g_client, g_topic_switch.c_str(), 1);
//                 esp_mqtt_client_subscribe(g_client, g_topic_update.c_str(), 1);
//                 esp_mqtt_client_subscribe(g_client, g_topic_status_cmd.c_str(), 1);
//                 publish_status();
//                 break;

//             case MQTT_EVENT_DISCONNECTED:
//                 ESP_LOGW(TAG, "MQTT disconnected – will auto-reconnect");
//                 break;

//             case MQTT_EVENT_DATA: {
//                 /* Null-terminate for easier string ops */
//                 char topic_buf[128];
//                 const size_t topic_len = static_cast<size_t>(event->topic_len);
//                 if (topic_len >= sizeof(topic_buf)) break;
                
//                 std::memcpy(topic_buf, event->topic, topic_len);
//                 topic_buf[topic_len] = '\0';

//                 ESP_LOGI(TAG, "MQTT cmd: %s", topic_buf);

//                 if (std::strcmp(topic_buf, g_topic_switch.c_str()) == 0) {
//                     const auto target = slot_from_payload(event->data, event->data_len);
//                     ESP_LOGI(TAG, "Switch command → %s",
//                              boot_ctrl::slot_name(target).data());
//                     (void)boot_ctrl::switch_to(target); // reboots on success

//                 } else if (std::strcmp(topic_buf, g_topic_update.c_str()) == 0) {
//                     const auto target = slot_from_payload(event->data, event->data_len);
//                     ESP_LOGI(TAG, "OTA update command for slot %s",
//                              boot_ctrl::slot_name(target).data());
//                     (void)ota_manager::check_and_update(g_cfg.ota_cfg, target);

//                 } else if (std::strcmp(topic_buf, g_topic_status_cmd.c_str()) == 0) {
//                     publish_status();
//                 }
//                 break;
//             }

//             case MQTT_EVENT_ERROR:
//                 ESP_LOGE(TAG, "MQTT error");
//                 break;

//             default:
//                 break;
//         }
//     }
// } // anonymous namespace

// /* ── Public API ─────────────────────────────────────────────────────────── */
// namespace net_cmd {
//     esp_err_t mqtt_start(const MqttConfig &cfg)
//     {
//         g_cfg = cfg;

//         /* C++17: Build topic strings without std::format */
//         build_topic(g_topic_switch, "devices/", cfg.device_id, "/cmd/switch_fw");
//         build_topic(g_topic_update, "devices/", cfg.device_id, "/cmd/update_fw");
//         build_topic(g_topic_status_cmd, "devices/", cfg.device_id, "/cmd/status");
//         build_topic(g_topic_status_pub, "devices/", cfg.device_id, "/status");

//         esp_mqtt_client_config_t mqtt_cfg{};
//         mqtt_cfg.broker.address.uri                     = cfg.broker_uri.data();
//         mqtt_cfg.broker.verification.certificate        = cfg.ca_cert_pem.data();
//         mqtt_cfg.credentials.authentication.certificate = cfg.client_cert_pem.data();
//         mqtt_cfg.credentials.authentication.key         = cfg.client_key_pem.data();

//         g_client = esp_mqtt_client_init(&mqtt_cfg);
//         if (!g_client) {
//             ESP_LOGE(TAG, "Failed to init MQTT client");
//             return ESP_FAIL;
//         }

//         esp_mqtt_client_register_event(g_client, MQTT_EVENT_ANY,
//                                        mqtt_event_handler, nullptr);
//         esp_mqtt_client_start(g_client);

//         ESP_LOGI(TAG, "MQTT client started, broker: %s", cfg.broker_uri.data());
//         return ESP_OK;
//     }

//     void mqtt_stop()
//     {
//         if (g_client) {
//             esp_mqtt_client_stop(g_client);
//             esp_mqtt_client_destroy(g_client);
//             g_client = nullptr;
//         }
//     }
// } // namespace net_cmd


/*
mqtt_cmd.cpp
MQTT subscriber with zero-heap JSON generation
*/
#include "net_cmd.hpp"
#include <cstring>
#include <array>
#include "esp_log.h"
#include "esp_app_desc.h"
#include "mqtt_client.h"
#include "boot_ctrl.hpp"
#include "ota_manager.hpp"
#include "etl/string.h"

static const char *TAG = "mqtt_cmd";

namespace {
net_cmd::MqttConfig g_cfg;
esp_mqtt_client_handle_t g_client{nullptr};

/* ETL fixed strings for topics (no heap) */
etl::string_fixed<96> g_topic_switch;
etl::string_fixed<96> g_topic_update;
etl::string_fixed<96> g_topic_status_cmd;
etl::string_fixed<96> g_topic_status_pub;

/* JSON buffer for status (no malloc) */
constexpr size_t kJsonBufSize = 256;
using JsonBuffer = std::array<char, kJsonBufSize>;

void build_topic(etl::string_fixed<96>& out,
                 std::string_view prefix,
                 std::string_view device_id,
                 std::string_view suffix) noexcept
{
    out.clear();
    out.append(prefix.data(), prefix.size());
    out.append(device_id.data(), device_id.size());
    out.append(suffix.data(), suffix.size());
}

/* Manual JSON builder (same as http_cmd.cpp) */
void json_start_object(JsonBuffer &buf, size_t &pos) noexcept {
    if (pos < kJsonBufSize - 1) buf[pos++] = '{';
}
void json_end_object(JsonBuffer &buf, size_t &pos) noexcept {
    if (pos < kJsonBufSize - 1) buf[pos++] = '}';
}
void json_add_string(JsonBuffer &buf, size_t &pos,
                     const char *key, const char *value) noexcept {
    int written = std::snprintf(buf.data() + pos, kJsonBufSize - pos,
                                "\"%s\":\"%s\"", key, value);
    if (written > 0 && pos + written < kJsonBufSize) pos += written;
}
void json_add_comma(JsonBuffer &buf, size_t &pos) noexcept {
    if (pos < kJsonBufSize - 1) buf[pos++] = ',';
}
void json_finalize(JsonBuffer &buf, size_t pos) noexcept {
    if (pos < kJsonBufSize) buf[pos] = '\0';
}

/* ── Publish device status (no cJSON) ──────────────────────────────────── */
void publish_status()
{
    if (!g_client) return;
    const esp_app_desc_t *desc = esp_app_get_description();
    
    JsonBuffer json_buf{};
    size_t pos = 0;
    
    json_start_object(json_buf, pos);
    json_add_string(json_buf, pos, "running_slot",
                    boot_ctrl::slot_name(boot_ctrl::running_slot()).data());
    json_add_comma(json_buf, pos);
    json_add_string(json_buf, pos, "version", desc->version);
    json_end_object(json_buf, pos);
    json_finalize(json_buf, pos);
    
    esp_mqtt_client_publish(g_client,
                            g_topic_status_pub.c_str(),
                            json_buf.data(),
                            0, 1, 0);
}

/* ── Decode slot from payload ──────────────────────────────────────────── */
[[nodiscard]] boot_ctrl::Slot slot_from_payload(const char *data, int len) noexcept
{
    return (len > 0 && (data[0] == 'B' || data[0] == 'b'))
        ? boot_ctrl::Slot::FirmwareB
        : boot_ctrl::Slot::FirmwareA;
}

/* ── MQTT event handler ────────────────────────────────────────────────── */
void mqtt_event_handler(void *arg,
                        esp_event_base_t base,
                        int32_t event_id,
                        void *event_data)
{
    auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            esp_mqtt_client_subscribe(g_client, g_topic_switch.c_str(), 1);
            esp_mqtt_client_subscribe(g_client, g_topic_update.c_str(), 1);
            esp_mqtt_client_subscribe(g_client, g_topic_status_cmd.c_str(), 1);
            publish_status();
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected – will auto-reconnect");
            break;
            
        case MQTT_EVENT_DATA: {
            char topic_buf[128];
            const size_t topic_len = static_cast<size_t>(event->topic_len);
            if (topic_len >= sizeof(topic_buf)) break;
            
            std::memcpy(topic_buf, event->topic, topic_len);
            topic_buf[topic_len] = '\0';
            
            ESP_LOGI(TAG, "MQTT cmd: %s", topic_buf);
            
            if (std::strcmp(topic_buf, g_topic_switch.c_str()) == 0) {
                const auto target = slot_from_payload(event->data, event->data_len);
                ESP_LOGI(TAG, "Switch command → %s",
                         boot_ctrl::slot_name(target).data());
                (void)boot_ctrl::switch_to(target);
                
            } else if (std::strcmp(topic_buf, g_topic_update.c_str()) == 0) {
                const auto target = slot_from_payload(event->data, event->data_len);
                ESP_LOGI(TAG, "OTA update command for slot %s",
                         boot_ctrl::slot_name(target).data());
                (void)ota_manager::check_and_update(g_cfg.ota_cfg, target);
                
            } else if (std::strcmp(topic_buf, g_topic_status_cmd.c_str()) == 0) {
                publish_status();
            }
            break;
        }
        
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            break;
            
        default:
            break;
    }
}

} // anonymous namespace

/* ── Public API ────────────────────────────────────────────────────────── */
namespace net_cmd {

esp_err_t mqtt_start(const MqttConfig &cfg)
{
    g_cfg = cfg;
    
    build_topic(g_topic_switch, "devices/", cfg.device_id, "/cmd/switch_fw");
    build_topic(g_topic_update, "devices/", cfg.device_id, "/cmd/update_fw");
    build_topic(g_topic_status_cmd, "devices/", cfg.device_id, "/cmd/status");
    build_topic(g_topic_status_pub, "devices/", cfg.device_id, "/status");
    
    esp_mqtt_client_config_t mqtt_cfg{};
    mqtt_cfg.broker.address.uri = cfg.broker_uri.data();
    mqtt_cfg.broker.verification.certificate = cfg.ca_cert_pem.data();
    mqtt_cfg.credentials.authentication.certificate = cfg.client_cert_pem.data();
    mqtt_cfg.credentials.authentication.key = cfg.client_key_pem.data();
    
    g_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!g_client) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        return ESP_FAIL;
    }
    
    esp_mqtt_client_register_event(g_client, MQTT_EVENT_ANY,
                                   mqtt_event_handler, nullptr);
    esp_mqtt_client_start(g_client);
    
    ESP_LOGI(TAG, "MQTT client started, broker: %s", cfg.broker_uri.data());
    return ESP_OK;
}

void mqtt_stop()
{
    if (g_client) {
        esp_mqtt_client_stop(g_client);
        esp_mqtt_client_destroy(g_client);
        g_client = nullptr;
    }
}

} // namespace net_cmd