// /*
//  * http_cmd.cpp
//  *
//  * Tiny REST server for runtime firmware switching and OTA triggering.
//  * Protected by HMAC-SHA256 request signing.
//  */

// #include "net_cmd.hpp"

// #include <array>
// #include <charconv>
// #include <cstring>
// #include <ctime>
// #include <string>

// #include "esp_http_server.h"
// #include "esp_log.h"
// #include "esp_app_desc.h"
// // #include <mbedtls/md.h>
// #include <mbedtls/tf-psa-crypto/md.h>
// #include "cJSON.h"
// #include "boot_ctrl.hpp"
// #include "ota_manager.hpp"

// static const char *TAG = "http_cmd";

// namespace {

// /* Stored config – set once at http_start() */
// net_cmd::HttpConfig g_cfg;
// httpd_handle_t      g_server{nullptr};

// /* ── HMAC-SHA256 verification ───────────────────────────────────────────── */

// /**
//  * Expected Authorization header:
//  *   HMAC-SHA256 <hex(HMAC-SHA256(secret, "<METHOD>\n<PATH>\n<unix_ts>"))>
//  *
//  * The Unix timestamp in the header must be within ±30 s of device time.
//  */
// [[nodiscard]] bool verify_hmac(httpd_req_t *req,
//                                std::string_view secret) noexcept
// {
//     /* Extract Authorization header */
//     char auth_hdr[128]{};
//     if (httpd_req_get_hdr_value_str(req, "Authorization",
//                                     auth_hdr, sizeof(auth_hdr)) != ESP_OK)
//     {
//         return false;
//     }

//     /* Parse: "HMAC-SHA256 <hex>" */
//     const char *prefix = "HMAC-SHA256 ";
//     if (strncmp(auth_hdr, prefix, strlen(prefix)) != 0) return false;
//     const char *recv_hex = auth_hdr + strlen(prefix);

//     /* Extract timestamp from a custom header "X-Timestamp" */
//     char ts_str[32]{};
//     if (httpd_req_get_hdr_value_str(req, "X-Timestamp",
//                                     ts_str, sizeof(ts_str)) != ESP_OK)
//     {
//         return false;
//     }

//     /* Verify timestamp freshness (±30 s) */
//     uint32_t recv_ts = 0;
//     std::from_chars(ts_str, ts_str + strlen(ts_str), recv_ts);
//     const uint32_t now = static_cast<uint32_t>(std::time(nullptr));
//     if (std::abs(static_cast<int32_t>(now - recv_ts)) > 30) {
//         ESP_LOGW(TAG, "HMAC timestamp too old/far-future: %lu vs now %lu",
//                  recv_ts, now);
//         return false;
//     }

//     /* Build the expected message: "<METHOD>\n<URI>\n<TS>" */
//     char method[8]{};
//     char uri[128]{};
//     /* httpd_req_t exposes method as a string and uri directly */
//     snprintf(method, sizeof(method), "%s", http_method_str(
//         static_cast<http_method>(req->method)));
//     snprintf(uri, sizeof(uri), "%s", req->uri);

//     char message[256]{};
//     snprintf(message, sizeof(message), "%s\n%s\n%s", method, uri, ts_str);

//     /* Compute expected HMAC */
//     std::array<uint8_t, 32> mac{};
//     mbedtls_md_hmac(
//         mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
//         reinterpret_cast<const uint8_t *>(secret.data()), secret.size(),
//         reinterpret_cast<const uint8_t *>(message), strlen(message),
//         mac.data());

//     /* Encode to hex */
//     char expected_hex[65]{};
//     for (int i = 0; i < 32; ++i)
//         snprintf(expected_hex + 2 * i, 3, "%02x", mac[i]);

//     /* Constant-time compare */
//     if (strlen(recv_hex) != 64) return false;
//     uint8_t diff = 0;
//     for (int i = 0; i < 64; ++i)
//         diff |= static_cast<uint8_t>(recv_hex[i] ^ expected_hex[i]);

//     return diff == 0;
// }

// /* ── GET /status ─────────────────────────────────────────────────────────── */

// esp_err_t handle_status(httpd_req_t *req)
// {
//     const esp_app_desc_t *desc = esp_app_get_description();

//     cJSON *root = cJSON_CreateObject();
//     cJSON_AddStringToObject(root, "running_slot",
//         boot_ctrl::slot_name(boot_ctrl::running_slot()).data());
//     cJSON_AddStringToObject(root, "version", desc->version);
//     cJSON_AddStringToObject(root, "project_name", desc->project_name);

//     char *json_str = cJSON_PrintUnformatted(root);
//     cJSON_Delete(root);

//     httpd_resp_set_type(req, "application/json");
//     httpd_resp_sendstr(req, json_str);
//     free(json_str);
//     return ESP_OK;
// }

// /* ── POST /cmd/switch?fw=A|B ─────────────────────────────────────────────── */

// esp_err_t handle_switch(httpd_req_t *req)
// {
//     if (!verify_hmac(req, g_cfg.hmac_secret)) {
//         httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Bad HMAC");
//         return ESP_FAIL;
//     }

//     char query[32]{};
//     httpd_req_get_url_query_str(req, query, sizeof(query));

//     char fw_param[4]{};
//     httpd_query_key_value(query, "fw", fw_param, sizeof(fw_param));

//     const auto target = (fw_param[0] == 'B' || fw_param[0] == 'b')
//                         ? boot_ctrl::Slot::FirmwareB
//                         : boot_ctrl::Slot::FirmwareA;

//     httpd_resp_sendstr(req, "Switching firmware...");

//     auto result = boot_ctrl::switch_to(target);
//     /* switch_to() reboots on success; if we reach here it failed */
//     if (!result) {
//         ESP_LOGE(TAG, "Switch failed");
//     }
//     return ESP_OK;
// }

// /* ── POST /cmd/update?slot=A|B ───────────────────────────────────────────── */

// esp_err_t handle_update(httpd_req_t *req)
// {
//     if (!verify_hmac(req, g_cfg.hmac_secret)) {
//         httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Bad HMAC");
//         return ESP_FAIL;
//     }

//     char query[32]{};
//     httpd_req_get_url_query_str(req, query, sizeof(query));

//     char slot_param[4]{};
//     httpd_query_key_value(query, "slot", slot_param, sizeof(slot_param));

//     const auto target = (slot_param[0] == 'B' || slot_param[0] == 'b')
//                         ? boot_ctrl::Slot::FirmwareB
//                         : boot_ctrl::Slot::FirmwareA;

//     httpd_resp_sendstr(req, "OTA update triggered...");

//     auto result = ota_manager::check_and_update(g_cfg.ota_cfg, target);
//     if (!result && result.error() != ota_manager::Error::VersionCurrent) {
//         ESP_LOGE(TAG, "OTA update failed");
//     }
//     return ESP_OK;
// }

// } // anonymous namespace

// /* ── Public API ─────────────────────────────────────────────────────────── */

// namespace net_cmd {

// esp_err_t http_start(const HttpConfig &cfg)
// {
//     g_cfg = cfg;

//     httpd_config_t server_cfg = HTTPD_DEFAULT_CONFIG();
//     server_cfg.server_port    = cfg.port;
//     server_cfg.lru_purge_enable = true;

//     if (httpd_start(&g_server, &server_cfg) != ESP_OK) {
//         ESP_LOGE(TAG, "Failed to start HTTP server");
//         return ESP_FAIL;
//     }

//     static const httpd_uri_t status_uri = {
//         .uri      = "/status",
//         .method   = HTTP_GET,
//         .handler  = handle_status,
//         .user_ctx = nullptr,
//     };
//     static const httpd_uri_t switch_uri = {
//         .uri      = "/cmd/switch",
//         .method   = HTTP_POST,
//         .handler  = handle_switch,
//         .user_ctx = nullptr,
//     };
//     static const httpd_uri_t update_uri = {
//         .uri      = "/cmd/update",
//         .method   = HTTP_POST,
//         .handler  = handle_update,
//         .user_ctx = nullptr,
//     };

//     httpd_register_uri_handler(g_server, &status_uri);
//     httpd_register_uri_handler(g_server, &switch_uri);
//     httpd_register_uri_handler(g_server, &update_uri);

//     ESP_LOGI(TAG, "HTTP command server started on port %u", cfg.port);
//     return ESP_OK;
// }

// void http_stop()
// {
//     if (g_server) {
//         httpd_stop(g_server);
//         g_server = nullptr;
//     }
// }

// } // namespace net_cmd


/*
 * http_cmd.cpp
 * Tiny REST server for runtime firmware switching and OTA triggering.
 * Protected by HMAC-SHA256 request signing with zero-heap JSON generation (manual string building)
*/
#include "net_cmd.hpp"
#include <cstdint>
#include <cstring>
#include <charconv>
#include <array>
#include <ctime>
#include <psa/crypto.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "boot_ctrl.hpp"
#include "ota_manager.hpp"

static const char *TAG = "http_cmd";

namespace {
net_cmd::HttpConfig g_cfg;
httpd_handle_t g_server{nullptr};

/* ── JSON Response Buffer (fixed size, no malloc) ──────────────────────── */
constexpr size_t kJsonBufSize = 512;
using JsonBuffer = std::array<char, kJsonBufSize>;

/* ── Manual JSON Builder (no cJSON, no malloc) ─────────────────────────── */
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
    if (written > 0 && pos + written < kJsonBufSize) {
        pos += written;
    }
}

void json_add_comma(JsonBuffer &buf, size_t &pos) noexcept {
    if (pos < kJsonBufSize - 1) buf[pos++] = ',';
}

void json_finalize(JsonBuffer &buf, size_t pos) noexcept {
    if (pos < kJsonBufSize) buf[pos] = '\0';
}

/* ── PSA helper utilities ───────────────────────────────────────────────── */
static bool ensure_psa_initialized() noexcept
{
    static bool initialized = false;
    if (initialized) {
        return true;
    }

    if (psa_crypto_init() == PSA_SUCCESS) {
        initialized = true;
        return true;
    }
    return false;
}

static int hex_value(char c) noexcept
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex_to_bytes(std::string_view hex,
                         std::array<uint8_t, 32> &out) noexcept
{
    if (hex.size() != out.size() * 2) {
        return false;
    }

    for (size_t i = 0; i < out.size(); ++i) {
        int hi = hex_value(hex[2 * i]);
        int lo = hex_value(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

/* ── HMAC-SHA256 verification ──────────────────────────────────────────── */
[[nodiscard]] bool verify_hmac(httpd_req_t *req,
                               std::string_view secret) noexcept
{
    char auth_hdr[128]{};
    if (httpd_req_get_hdr_value_str(req, "Authorization",
                                    auth_hdr, sizeof(auth_hdr)) != ESP_OK) {
        return false;
    }

    const char *prefix = "HMAC-SHA256 ";
    if (strncmp(auth_hdr, prefix, strlen(prefix)) != 0) return false;
    const char *recv_hex = auth_hdr + strlen(prefix);

    char ts_str[32]{};
    if (httpd_req_get_hdr_value_str(req, "X-Timestamp",
                                    ts_str, sizeof(ts_str)) != ESP_OK) {
        return false;
    }

    uint32_t recv_ts = 0;
    std::from_chars(ts_str, ts_str + strlen(ts_str), recv_ts);
    const uint32_t now = static_cast<uint32_t>(std::time(nullptr));
    if (std::abs(static_cast<int32_t>(now - recv_ts)) > 30) {
        ESP_LOGW(TAG, "HMAC timestamp too old/far-future: %lu vs now %lu",
                 recv_ts, now);
        return false;
    }

    char method[8]{};
    char uri[128]{};
    snprintf(method, sizeof(method), "%s",
             http_method_str(static_cast<http_method>(req->method)));
    snprintf(uri, sizeof(uri), "%.*s", static_cast<int>(sizeof(uri) - 1), req->uri);

    char message[256]{};
    snprintf(message, sizeof(message), "%s\n%s\n%s", method, uri, ts_str);

    if (!ensure_psa_initialized()) {
        return false;
    }

    psa_key_attributes_t attrs = psa_key_attributes_init();
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&attrs, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attrs, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attrs, static_cast<uint32_t>(secret.size() * 8));

    mbedtls_svc_key_id_t key_id{};
    if (psa_import_key(&attrs,
                       reinterpret_cast<const uint8_t*>(secret.data()),
                       secret.size(),
                       &key_id) != PSA_SUCCESS) {
        return false;
    }

    std::array<uint8_t, 32> recv_mac{};
    if (!hex_to_bytes(recv_hex, recv_mac)) {
        psa_destroy_key(key_id);
        return false;
    }

    psa_status_t status = psa_mac_verify(key_id,
                                        PSA_ALG_HMAC(PSA_ALG_SHA_256),
                                        reinterpret_cast<const uint8_t*>(message),
                                        strlen(message),
                                        recv_mac.data(),
                                        recv_mac.size());
    psa_destroy_key(key_id);

    return status == PSA_SUCCESS;
}

/* ── GET /status (manual JSON, no cJSON) ───────────────────────────────── */
esp_err_t handle_status(httpd_req_t *req)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    
    JsonBuffer json_buf{};
    size_t pos = 0;
    
    json_start_object(json_buf, pos);
    json_add_string(json_buf, pos, "running_slot",
                    boot_ctrl::slot_name(boot_ctrl::running_slot()).data());
    json_add_comma(json_buf, pos);
    json_add_string(json_buf, pos, "version", desc->version);
    json_add_comma(json_buf, pos);
    json_add_string(json_buf, pos, "project_name", desc->project_name);
    json_end_object(json_buf, pos);
    json_finalize(json_buf, pos);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_buf.data());
    return ESP_OK;
}

/* ── POST /cmd/switch?fw=A|B ───────────────────────────────────────────── */
esp_err_t handle_switch(httpd_req_t *req)
{
    if (!verify_hmac(req, g_cfg.hmac_secret)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Bad HMAC");
        return ESP_FAIL;
    }
    
    char query[32]{};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    
    char fw_param[4]{};
    httpd_query_key_value(query, "fw", fw_param, sizeof(fw_param));
    
    const auto target = (fw_param[0] == 'B' || fw_param[0] == 'b')
                        ? boot_ctrl::Slot::FirmwareB
                        : boot_ctrl::Slot::FirmwareA;
    
    httpd_resp_sendstr(req, "Switching firmware...");
    
    auto result = boot_ctrl::switch_to(target);
    if (!result) {
        ESP_LOGE(TAG, "Switch failed");
    }
    return ESP_OK;
}

/* ── POST /cmd/update?slot=A|B ─────────────────────────────────────────── */
esp_err_t handle_update(httpd_req_t *req)
{
    if (!verify_hmac(req, g_cfg.hmac_secret)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Bad HMAC");
        return ESP_FAIL;
    }
    
    char query[32]{};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    
    char slot_param[4]{};
    httpd_query_key_value(query, "slot", slot_param, sizeof(slot_param));
    
    const auto target = (slot_param[0] == 'B' || slot_param[0] == 'b')
                        ? boot_ctrl::Slot::FirmwareB
                        : boot_ctrl::Slot::FirmwareA;
    
    httpd_resp_sendstr(req, "OTA update triggered...");
    
    auto result = ota_manager::check_and_update(g_cfg.ota_cfg, target);
    if (!result && result.error() != ota_manager::Error::VersionCurrent) {
        ESP_LOGE(TAG, "OTA update failed");
    }
    return ESP_OK;
}

} // anonymous namespace

/* ── Public API ────────────────────────────────────────────────────────── */
namespace net_cmd {

esp_err_t http_start(const HttpConfig &cfg)
{
    g_cfg = cfg;
    httpd_config_t server_cfg = HTTPD_DEFAULT_CONFIG();
    server_cfg.server_port = cfg.port;
    server_cfg.lru_purge_enable = true;
    
    if (httpd_start(&g_server, &server_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }
    
    static const httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = handle_status,
        .user_ctx = nullptr,
    };
    static const httpd_uri_t switch_uri = {
        .uri = "/cmd/switch",
        .method = HTTP_POST,
        .handler = handle_switch,
        .user_ctx = nullptr,
    };
    static const httpd_uri_t update_uri = {
        .uri = "/cmd/update",
        .method = HTTP_POST,
        .handler = handle_update,
        .user_ctx = nullptr,
    };
    
    httpd_register_uri_handler(g_server, &status_uri);
    httpd_register_uri_handler(g_server, &switch_uri);
    httpd_register_uri_handler(g_server, &update_uri);
    
    ESP_LOGI(TAG, "HTTP command server started on port %u", cfg.port);
    return ESP_OK;
}

void http_stop()
{
    if (g_server) {
        httpd_stop(g_server);
        g_server = nullptr;
    }
}

} // namespace net_cmd