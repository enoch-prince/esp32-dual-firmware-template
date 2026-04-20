/*
 * wifi_manager.cpp
 *
 * Tries NVS credentials, then a hardcoded fallback, then falls back to an
 * AP provisioning loop.  The provisioning AP ("ESP32-Setup", open, no password)
 * serves a single-page credential form at http://192.168.4.1/.  Once valid
 * credentials are submitted the new SSID/password are saved to NVS and the
 * device continues normal boot in STA mode.
 */

#include "wifi_manager.hpp"
#include "wifi.hpp"

#include <cstring>
#include <cstdlib>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_mgr";

namespace {

/* ── NVS helpers ─────────────────────────────────────────────────────────── */

constexpr const char *NVS_NS   = "wifi_mgr";
constexpr const char *NVS_SSID = "ssid";
constexpr const char *NVS_PASS = "pass";

char g_ssid[32]{};
char g_pass[64]{};

bool load_nvs_credentials()
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    size_t ssid_len = sizeof(g_ssid);
    size_t pass_len = sizeof(g_pass);
    const bool ok =
        nvs_get_str(h, NVS_SSID, g_ssid, &ssid_len) == ESP_OK &&
        nvs_get_str(h, NVS_PASS, g_pass, &pass_len) == ESP_OK &&
        g_ssid[0] != '\0';
    nvs_close(h);
    return ok;
}

void save_nvs_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_SSID, ssid);
    nvs_set_str(h, NVS_PASS, pass);
    nvs_commit(h);
    nvs_close(h);
}

/* ── URL decoder (for multipart form body) ───────────────────────────────── */

static void url_decode(const char *src, char *dst, size_t dst_len)
{
    size_t i = 0;
    while (*src && i < dst_len - 1) {
        if (*src == '+') {
            dst[i++] = ' ';
            ++src;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], '\0'};
            dst[i++] = static_cast<char>(std::strtol(hex, nullptr, 16));
            src += 3;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static void parse_form_field(const char *body,
                              const char *key,
                              char *out, size_t out_len)
{
    const char *p = std::strstr(body, key);
    if (!p) { out[0] = '\0'; return; }
    p += std::strlen(key);
    char encoded[128]{};
    size_t i = 0;
    while (*p && *p != '&' && i < sizeof(encoded) - 1) encoded[i++] = *p++;
    encoded[i] = '\0';
    url_decode(encoded, out, out_len);
}

/* ── Provisioning AP + HTTP server ───────────────────────────────────────── */

static constexpr const char PROV_FORM_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Wi-Fi Setup</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:320px;margin:60px auto;padding:0 16px}"
    "h2{margin-bottom:20px}"
    "label{display:block;margin-bottom:4px;font-size:.9rem;color:#555}"
    "input{width:100%;padding:8px;margin-bottom:14px;box-sizing:border-box;"
          "border:1px solid #ccc;border-radius:4px;font-size:1rem}"
    "button{width:100%;padding:10px;background:#0066cc;color:#fff;"
           "border:none;border-radius:4px;font-size:1rem;cursor:pointer}"
    "button:hover{background:#0055aa}"
    ".err{color:#c00;font-size:.85rem;margin-bottom:10px}"
    "</style></head><body>"
    "<h2>ESP32 Wi-Fi Setup</h2>"
    "<form method='POST' action='/provision'>"
    "<label>Network SSID</label>"
    "<input type='text' name='ssid' required maxlength='31' autocomplete='off'>"
    "<label>Password</label>"
    "<input type='password' name='pass' maxlength='63' autocomplete='off'>"
    "<button type='submit'>Connect</button>"
    "</form></body></html>";

static constexpr const char PROV_WAIT_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Connecting…</title>"
    "</head><body style='font-family:sans-serif;max-width:320px;margin:60px auto;padding:0 16px'>"
    "<h2>Connecting to Wi-Fi…</h2>"
    "<p>The device is trying your credentials. "
    "If it succeeds it will leave setup mode — "
    "reconnect to your normal network and continue.</p>"
    "<p>If the credentials are wrong, the setup AP will reappear shortly.</p>"
    "</body></html>";

static char                s_prov_ssid[32]{};
static char                s_prov_pass[64]{};
static EventGroupHandle_t  s_prov_event{nullptr};
constexpr EventBits_t      kSubmitted = BIT0;

static esp_err_t handle_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PROV_FORM_HTML, sizeof(PROV_FORM_HTML) - 1);
    return ESP_OK;
}

static esp_err_t handle_post(httpd_req_t *req)
{
    char body[256]{};
    const int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[len] = '\0';

    parse_form_field(body, "ssid=", s_prov_ssid, sizeof(s_prov_ssid));
    parse_form_field(body, "pass=", s_prov_pass, sizeof(s_prov_pass));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PROV_WAIT_HTML, sizeof(PROV_WAIT_HTML) - 1);

    xEventGroupSetBits(s_prov_event, kSubmitted);
    return ESP_OK;
}

static esp_err_t run_provisioning()
{
    ESP_LOGI(TAG, "Starting provisioning AP \"ESP32-Setup\" (open)");
    ESP_LOGI(TAG, "Connect to the AP and visit http://192.168.4.1/");

    esp_wifi_stop();
    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg{};
    constexpr const char *AP_SSID = "ESP32-Setup";
    std::strncpy(reinterpret_cast<char *>(ap_cfg.ap.ssid),
                 AP_SSID, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len      = static_cast<uint8_t>(std::strlen(AP_SSID));
    ap_cfg.ap.authmode      = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_prov_event = xEventGroupCreate();
    s_prov_ssid[0] = '\0';
    s_prov_pass[0] = '\0';

    httpd_handle_t server = nullptr;
    httpd_config_t hcfg   = HTTPD_DEFAULT_CONFIG();
    hcfg.server_port      = 80;

    if (httpd_start(&server, &hcfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start provisioning HTTP server");
        vEventGroupDelete(s_prov_event);
        s_prov_event = nullptr;
        return ESP_FAIL;
    }

    const httpd_uri_t get_uri  = {"/",          HTTP_GET,  handle_get,  nullptr};
    const httpd_uri_t post_uri = {"/provision", HTTP_POST, handle_post, nullptr};
    httpd_register_uri_handler(server, &get_uri);
    httpd_register_uri_handler(server, &post_uri);

    xEventGroupWaitBits(s_prov_event, kSubmitted, pdFALSE, pdFALSE, portMAX_DELAY);

    httpd_stop(server);
    vEventGroupDelete(s_prov_event);
    s_prov_event = nullptr;

    esp_wifi_stop();

    std::strncpy(g_ssid, s_prov_ssid, sizeof(g_ssid) - 1);
    std::strncpy(g_pass, s_prov_pass, sizeof(g_pass) - 1);

    return ESP_OK;
}

/* ── STA connection attempt ──────────────────────────────────────────────── */

static bool try_connect(const char *ssid, const char *pass,
                        uint32_t timeout_ms, uint8_t max_retries)
{
    if (!ssid || ssid[0] == '\0') return false;
    ESP_LOGI(TAG, "Trying SSID: %s", ssid);
    const wifi::Config wcfg{
        .ssid               = ssid,
        .password           = pass ? pass : "",
        .connect_timeout_ms = timeout_ms,
        .max_retries        = max_retries,
    };
    return wifi::connect(wcfg) == ESP_OK;
}

} // anonymous namespace

/* ── Public API ─────────────────────────────────────────────────────────── */

namespace wifi_manager {

esp_err_t connect(const Config &cfg)
{
    // 1. NVS-stored credentials
    if (load_nvs_credentials()) {
        if (try_connect(g_ssid, g_pass, cfg.connect_timeout_ms, cfg.max_retries)) {
            ESP_LOGI(TAG, "Connected with stored credentials");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Stored credentials failed");
    }

    // 2. Hardcoded fallback
    if (!cfg.fallback_ssid.empty()) {
        char ssid[32]{}, pass[64]{};
        std::strncpy(ssid, cfg.fallback_ssid.data(),     sizeof(ssid) - 1);
        std::strncpy(pass, cfg.fallback_password.data(), sizeof(pass) - 1);
        if (try_connect(ssid, pass, cfg.connect_timeout_ms, cfg.max_retries)) {
            ESP_LOGI(TAG, "Connected with fallback credentials — saving to NVS");
            save_nvs_credentials(ssid, pass);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Fallback credentials failed");
    }

    // 3. AP provisioning loop — keeps retrying until valid credentials are given
    while (true) {
        if (run_provisioning() != ESP_OK) return ESP_FAIL;

        if (try_connect(g_ssid, g_pass, cfg.connect_timeout_ms, cfg.max_retries)) {
            ESP_LOGI(TAG, "Provisioning succeeded — saving credentials to NVS");
            save_nvs_credentials(g_ssid, g_pass);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Provisioned credentials rejected — restarting AP");
    }
}

} // namespace wifi_manager
