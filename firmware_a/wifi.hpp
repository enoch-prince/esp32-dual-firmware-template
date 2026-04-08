#pragma once

/*
 * wifi.hpp
 *
 * Simple STA-mode Wi-Fi helper that blocks until connected or times out.
 */

#include <string_view>
#include "esp_err.h"

namespace wifi {

struct Config {
    std::string_view ssid;
    std::string_view password;
    uint32_t         connect_timeout_ms{15'000};
    uint8_t          max_retries{5};
};

/**
 * @brief  Initialise NVS + netif, connect to the configured AP.
 *         Blocks until IP is obtained or timeout expires.
 * @return ESP_OK on success, ESP_ERR_TIMEOUT or ESP_FAIL otherwise.
 */
[[nodiscard]] esp_err_t connect(const Config &cfg);

void disconnect();

} // namespace wifi
