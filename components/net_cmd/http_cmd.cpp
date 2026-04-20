/*
 * http_cmd.cpp
 * Tiny REST server for runtime firmware switching and OTA triggering.
 * Protected by HMAC-SHA256 request signing with zero-heap JSON generation (manual string building)
*/
#include "net_cmd.hpp"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <array>
#include <psa/crypto.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "boot_ctrl.hpp"
#include "ota_manager.hpp"
#include "nvs.h"

static const char *TAG = "http_cmd";

namespace {
net_cmd::HttpConfig         g_cfg;
httpd_handle_t              g_server{nullptr};
std::atomic<bool>           g_ota_in_progress{false};

/* ── Runtime-configurable OTA manifest URL ───────────────────────────────── */

static char g_manifest_url_buf[256]{};

static const char *ota_nvs_namespace()
{
    return (boot_ctrl::running_slot() == boot_ctrl::Slot::FirmwareA)
           ? "ota_fw_a" : "ota_fw_b";
}

static void load_manifest_url_nvs()
{
    nvs_handle_t h;
    if (nvs_open(ota_nvs_namespace(), NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(g_manifest_url_buf);
    if (nvs_get_str(h, "manifest_url", g_manifest_url_buf, &len) == ESP_OK
            && g_manifest_url_buf[0] != '\0') {
        g_cfg.ota_cfg.manifest_url = {g_manifest_url_buf, std::strlen(g_manifest_url_buf)};
        ESP_LOGI(TAG, "OTA URL loaded from NVS: %s", g_manifest_url_buf);
    }
    nvs_close(h);
}

static void save_manifest_url_nvs(const char *url)
{
    nvs_handle_t h;
    if (nvs_open(ota_nvs_namespace(), NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "manifest_url", url);
    nvs_commit(h);
    nvs_close(h);
}

/* ── OTA background task ───────────────────────────────────────────────── */
struct OtaTaskArg {
    ota_manager::Config cfg;
    boot_ctrl::Slot     target;
};

/* Static storage — safe because g_ota_in_progress prevents concurrent OTA.
 * Written by handle_update before xTaskCreate; read only by ota_task. */
static OtaTaskArg s_ota_arg;

static void ota_task(void * /*arg*/)
{
    auto result = ota_manager::check_and_update(s_ota_arg.cfg, s_ota_arg.target);
    if (!result && result.error() != ota_manager::Error::VersionCurrent) {
        ESP_LOGE(TAG, "OTA update failed (err=%u)",
                 static_cast<unsigned>(result.error()));
    }
    g_ota_in_progress.store(false);
    vTaskDelete(nullptr);
}

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

    /* NOTE: Timestamp freshness check is intentionally skipped in dev builds.
     * The ESP32 has no battery-backed RTC and SNTP is not configured, so
     * std::time() returns seconds-since-boot (near 0) while the server sends
     * actual Unix time — the ±30 s window would reject every request.
     * TODO: add esp_sntp sync in app_main() and re-enable this check for prod:
     *   uint32_t recv_ts = 0;
     *   std::from_chars(ts_str, ts_str + strlen(ts_str), recv_ts);
     *   const uint32_t now = static_cast<uint32_t>(std::time(nullptr));
     *   if (std::abs(static_cast<int32_t>(now - recv_ts)) > 30) return false;
     */

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

/* ── GET /ota/url ────────────────────────────────────────────────────────── */
esp_err_t handle_get_ota_url(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req,
                    g_cfg.ota_cfg.manifest_url.data(),
                    static_cast<ssize_t>(g_cfg.ota_cfg.manifest_url.size()));
    return ESP_OK;
}

/* ── POST /ota/url ───────────────────────────────────────────────────────── */
esp_err_t handle_set_ota_url(httpd_req_t *req)
{
    if (!verify_hmac(req, g_cfg.hmac_secret)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Bad HMAC");
        return ESP_FAIL;
    }

    char body[sizeof(g_manifest_url_buf)]{};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[len] = '\0';

    // Trim trailing whitespace
    while (len > 0 && (body[len - 1] == '\n' || body[len - 1] == '\r' || body[len - 1] == ' '))
        body[--len] = '\0';

    if (len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty URL");
        return ESP_FAIL;
    }

    std::strncpy(g_manifest_url_buf, body, sizeof(g_manifest_url_buf) - 1);
    g_manifest_url_buf[sizeof(g_manifest_url_buf) - 1] = '\0';
    g_cfg.ota_cfg.manifest_url = {g_manifest_url_buf, std::strlen(g_manifest_url_buf)};
    save_manifest_url_nvs(g_manifest_url_buf);

    ESP_LOGI(TAG, "OTA manifest URL updated: %s", g_manifest_url_buf);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, g_manifest_url_buf);
    return ESP_OK;
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

    /* Send response BEFORE switch_to() calls esp_restart() — the reboot
     * happens after kRebootDelayMs, giving lwIP time to flush the TCP ACK. */
    httpd_resp_sendstr(req, "Switching firmware – rebooting now");

    auto result = boot_ctrl::switch_to(target);
    if (!result) {
        /* switch_to() only returns on failure (already running that slot, or
         * partition not found).  We can't send a second response here because
         * the response was already sent above, so just log it. */
        ESP_LOGE(TAG, "switch_to failed (err=%u)",
                 static_cast<unsigned>(result.error()));
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

    if (g_ota_in_progress.exchange(true)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA already in progress");
        return ESP_FAIL;
    }

    char query[32]{};
    httpd_req_get_url_query_str(req, query, sizeof(query));

    char slot_param[4]{};
    httpd_query_key_value(query, "slot", slot_param, sizeof(slot_param));

    const auto target = (slot_param[0] == 'B' || slot_param[0] == 'b')
                        ? boot_ctrl::Slot::FirmwareB
                        : boot_ctrl::Slot::FirmwareA;

    s_ota_arg.cfg    = g_cfg.ota_cfg;
    s_ota_arg.target = target;

    /* Spawn background task then return immediately so the httpd task is free
     * to serve other requests.  ota_task() clears g_ota_in_progress when done
     * and calls vTaskDelete(nullptr) to clean itself up. */
    if (xTaskCreate(ota_task, "ota_bg", 16384, nullptr,
                    tskIDLE_PRIORITY + 3, nullptr) != pdPASS)
    {
        g_ota_in_progress.store(false);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Failed to start OTA task");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OTA triggered – running in background");
    return ESP_OK;
}

} // anonymous namespace

/* ── Public API ────────────────────────────────────────────────────────── */
namespace net_cmd {

esp_err_t http_start(const HttpConfig &cfg)
{
    g_cfg = cfg;
    load_manifest_url_nvs();  // override manifest_url from NVS if previously set

    httpd_config_t server_cfg = HTTPD_DEFAULT_CONFIG();
    server_cfg.server_port = cfg.port;
    server_cfg.lru_purge_enable = true;
    server_cfg.max_uri_handlers = 8;

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
    static const httpd_uri_t get_ota_url_uri = {
        .uri = "/ota/url",
        .method = HTTP_GET,
        .handler = handle_get_ota_url,
        .user_ctx = nullptr,
    };
    static const httpd_uri_t set_ota_url_uri = {
        .uri = "/ota/url",
        .method = HTTP_POST,
        .handler = handle_set_ota_url,
        .user_ctx = nullptr,
    };

    httpd_register_uri_handler(g_server, &status_uri);
    httpd_register_uri_handler(g_server, &switch_uri);
    httpd_register_uri_handler(g_server, &update_uri);
    httpd_register_uri_handler(g_server, &get_ota_url_uri);
    httpd_register_uri_handler(g_server, &set_ota_url_uri);
    
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