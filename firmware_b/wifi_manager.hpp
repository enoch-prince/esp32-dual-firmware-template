#pragma once

/*
 * wifi_manager.hpp
 *
 * Credential-aware Wi-Fi helper.  Connection priority:
 *   1. Credentials stored in NVS (namespace "wifi_mgr")
 *   2. Hardcoded fallback credentials from Config
 *   3. AP provisioning mode — serves an HTTP form on 192.168.4.1
 *      until valid credentials are submitted
 *
 * Blocks until connected.  Saves every successful set of credentials
 * back to NVS so they are used first on the next boot.
 */

#include <string_view>
#include "esp_err.h"

namespace wifi_manager {

struct Config {
    std::string_view fallback_ssid;
    std::string_view fallback_password;
    uint32_t         connect_timeout_ms{15'000};
    uint8_t          max_retries{5};
};

[[nodiscard]] esp_err_t connect(const Config &cfg);

} // namespace wifi_manager
