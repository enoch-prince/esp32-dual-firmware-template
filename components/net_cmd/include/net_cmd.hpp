#pragma once
/*
net_cmd.hpp
Two remote-command transports:
HttpCmd  – embeds a tiny HTTP server; exposes REST endpoints for
        switching firmware and triggering OTA updates.
        Requests must carry an HMAC-SHA256 token.
MqttCmd  – subscribes to command topics on an MQTT broker.
        Messages must carry a signed payload.

C++17 Compatible - Uses ETL strings for topics
*/
#include <string_view>
#include <cstdint>
#include "etl/string.h"
#include "boot_ctrl.hpp"
#include "ota_manager.hpp"

namespace net_cmd {
/* ── HTTP command server ─────────────────────────────────────────────────── */
struct HttpConfig {
    uint16_t         port{8080};
    std::string_view hmac_secret; ///< secret used to verify Authorization header
    ota_manager::Config ota_cfg;  ///< forwarded to ota_manager on update requests
};

/**
@brief  Start the HTTP command server.
*/
esp_err_t http_start(const HttpConfig &cfg);
void      http_stop();

/* ── MQTT command subscriber ─────────────────────────────────────────────── */
struct MqttConfig {
    std::string_view broker_uri;   ///< e.g. "mqtts://broker.example.com:8883"
    std::string_view ca_cert_pem;
    std::string_view client_cert_pem;
    std::string_view client_key_pem;
    std::string_view device_id;    ///< used to build topic path
    ota_manager::Config ota_cfg;
};

/**
@brief  Start the MQTT client and subscribe to command topics.
*/
esp_err_t mqtt_start(const MqttConfig &cfg);
void      mqtt_stop();

} // namespace net_cmd