// /*
//  * ota_manager.cpp
//  *
//  * HTTPS OTA with:
//  *   • CA-certificate pinning  (no public CA chain accepted)
//  *   • SHA-256 integrity check  (via esp_ota_end())
//  *   • ECDSA-P256 signature verification  (mbedTLS)
//  *   • Slot-aware writes  (always targets the standby or an explicit slot)
//  */

// #include "ota_manager.hpp"

// #include <array>
// #include <algorithm>
// #include <charconv>
// #include <cstring>
// #include <span>

// #include "esp_log.h"
// #include "esp_ota_ops.h"
// #include "esp_partition.h"
// #include "esp_http_client.h"
// #include "esp_app_desc.h"
// #include "cJSON.h"

// /* mbedTLS for ECDSA signature verification */
// #include "mbedtls/pk.h"
// #include "mbedtls/sha256.h"
// #include "mbedtls/base64.h"
// #include "mbedtls/error.h"

// static const char *TAG = "ota_manager";

// /* ── Internal helpers ───────────────────────────────────────────────────── */

// namespace {

// constexpr size_t kDownloadBufSize = 4096;

// /* ── Version comparison ─────────────────────────────────────────────────── */

// struct SemVer { uint32_t major{}, minor{}, patch{}; };

// [[nodiscard]] SemVer parse_semver(std::string_view v) noexcept
// {
//     SemVer sv;
//     auto it = v.begin();
//     auto parse_num = [&](uint32_t &out) {
//         auto [ptr, ec] = std::from_chars(it, v.end(), out);
//         it = ptr;
//         if (it != v.end() && *it == '.') ++it;
//     };
//     parse_num(sv.major);
//     parse_num(sv.minor);
//     parse_num(sv.patch);
//     return sv;
// }

// [[nodiscard]] bool is_newer(std::string_view remote, std::string_view local) noexcept
// {
//     const auto r = parse_semver(remote);
//     const auto l = parse_semver(local);
//     if (r.major != l.major) return r.major > l.major;
//     if (r.minor != l.minor) return r.minor > l.minor;
//     return r.patch > l.patch;
// }

// /* ── JSON manifest parser ───────────────────────────────────────────────── */

// [[nodiscard]] ota_manager::Result<ota_manager::FirmwareInfo>
// parse_manifest(const char *json_str, boot_ctrl::Slot target) noexcept
// {
//     using namespace ota_manager;

//     cJSON *root = cJSON_Parse(json_str);
//     if (!root) return std::unexpected(Error::ManifestParse);

//     /* Select the correct firmware object based on slot */
//     const char *fw_key = (target == boot_ctrl::Slot::FirmwareA)
//                          ? "firmware_a" : "firmware_b";

//     cJSON *fw = cJSON_GetObjectItemCaseSensitive(root, fw_key);
//     if (!cJSON_IsObject(fw)) {
//         cJSON_Delete(root);
//         return std::unexpected(Error::ManifestParse);
//     }

//     auto str_field = [&](const char *key) -> std::string {
//         cJSON *item = cJSON_GetObjectItemCaseSensitive(fw, key);
//         return cJSON_IsString(item) ? std::string(item->valuestring) : "";
//     };

//     FirmwareInfo info{
//         .version    = str_field("version"),
//         .url        = str_field("url"),
//         .sha256_hex = str_field("sha256"),
//         .sig_b64    = str_field("signature"),
//     };

//     cJSON *hwv = cJSON_GetObjectItemCaseSensitive(fw, "min_hw_version");
//     if (cJSON_IsNumber(hwv)) info.min_hw_ver = static_cast<uint32_t>(hwv->valueint);

//     cJSON_Delete(root);

//     if (info.version.empty() || info.url.empty() ||
//         info.sha256_hex.empty() || info.sig_b64.empty())
//     {
//         return std::unexpected(Error::ManifestParse);
//     }

//     return info;
// }

// /* ── HTTPS GET into a std::string (manifest only, small payload) ────────── */

// [[nodiscard]] ota_manager::Result<std::string>
// https_get_string(std::string_view url, std::string_view ca_pem,
//                  uint32_t timeout_ms) noexcept
// {
//     using namespace ota_manager;

//     esp_http_client_config_t cfg{};
//     cfg.url          = url.data();
//     cfg.cert_pem     = ca_pem.data();
//     cfg.timeout_ms   = static_cast<int>(timeout_ms);
//     cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;

//     esp_http_client_handle_t client = esp_http_client_init(&cfg);
//     if (!client) return std::unexpected(Error::HttpFetch);

//     std::string body;
//     body.reserve(1024);

//     if (esp_http_client_open(client, 0) != ESP_OK) {
//         esp_http_client_cleanup(client);
//         return std::unexpected(Error::TlsError);
//     }

//     esp_http_client_fetch_headers(client);

//     char buf[512];
//     int  len = 0;
//     while ((len = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
//         body.append(buf, static_cast<size_t>(len));
//     }

//     esp_http_client_close(client);
//     esp_http_client_cleanup(client);

//     return body;
// }

// /* ── ECDSA-P256 signature verification (mbedTLS) ────────────────────────── */

// [[nodiscard]] ota_manager::Result<void>
// verify_signature(std::span<const uint8_t> binary,
//                  std::string_view         sig_b64,
//                  std::string_view         pub_pem) noexcept
// {
//     using namespace ota_manager;

//     /* 1. Base64-decode the signature */
//     std::array<uint8_t, 256> sig_buf{};
//     size_t sig_len = 0;
//     if (mbedtls_base64_decode(sig_buf.data(), sig_buf.size(), &sig_len,
//             reinterpret_cast<const uint8_t *>(sig_b64.data()),
//             sig_b64.size()) != 0)
//     {
//         return std::unexpected(Error::SignatureInvalid);
//     }

//     /* 2. SHA-256 hash of the binary */
//     std::array<uint8_t, 32> hash{};
//     mbedtls_sha256(binary.data(), binary.size(), hash.data(), 0 /*is224=false*/);

//     /* 3. Load the public key */
//     mbedtls_pk_context pk;
//     mbedtls_pk_init(&pk);

//     int rc = mbedtls_pk_parse_public_key(
//         &pk,
//         reinterpret_cast<const uint8_t *>(pub_pem.data()),
//         pub_pem.size() + 1 /* include null terminator */);

//     if (rc != 0) {
//         mbedtls_pk_free(&pk);
//         return std::unexpected(Error::SignatureInvalid);
//     }

//     /* 4. Verify */
//     rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256,
//                            hash.data(), hash.size(),
//                            sig_buf.data(), sig_len);
//     mbedtls_pk_free(&pk);

//     if (rc != 0) {
//         char err[128];
//         mbedtls_strerror(rc, err, sizeof(err));
//         ESP_LOGE(TAG, "Signature verification failed: %s", err);
//         return std::unexpected(Error::SignatureInvalid);
//     }

//     return {};
// }

// /* ── OTA download + write loop ──────────────────────────────────────────── */

// [[nodiscard]] ota_manager::Result<void>
// download_and_flash(const ota_manager::FirmwareInfo &info,
//                    std::string_view                 ca_pem,
//                    std::string_view                 ecdsa_pub_pem,
//                    const esp_partition_t           *target_partition,
//                    uint32_t                         timeout_ms) noexcept
// {
//     using namespace ota_manager;

//     esp_ota_handle_t ota_handle{};
//     if (esp_ota_begin(target_partition, OTA_WITH_SEQUENTIAL_WRITES,
//                       &ota_handle) != ESP_OK)
//     {
//         return std::unexpected(Error::OtaBegin);
//     }

//     /* ── HTTP stream ──────────────────────────────────────────────────── */
//     esp_http_client_config_t cfg{};
//     cfg.url          = info.url.c_str();
//     cfg.cert_pem     = ca_pem.data();
//     cfg.timeout_ms   = static_cast<int>(timeout_ms);
//     cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
//     cfg.buffer_size  = kDownloadBufSize;

//     esp_http_client_handle_t client = esp_http_client_init(&cfg);
//     if (!client) {
//         esp_ota_abort(ota_handle);
//         return std::unexpected(Error::HttpFetch);
//     }

//     if (esp_http_client_open(client, 0) != ESP_OK) {
//         esp_http_client_cleanup(client);
//         esp_ota_abort(ota_handle);
//         return std::unexpected(Error::TlsError);
//     }

//     esp_http_client_fetch_headers(client);

//     /* We also accumulate the binary for signature verification.
//      * For a 1.75 MB max image this fits in flash-mapped memory via OTA;
//      * we re-read from the partition after writing. */
//     std::array<uint8_t, kDownloadBufSize> buf{};
//     int  len          = 0;
//     bool write_ok     = true;
//     int  total_written = 0;

//     while ((len = esp_http_client_read(
//                 client,
//                 reinterpret_cast<char *>(buf.data()),
//                 static_cast<int>(buf.size()))) > 0)
//     {
//         if (esp_ota_write(ota_handle, buf.data(),
//                           static_cast<size_t>(len)) != ESP_OK)
//         {
//             write_ok = false;
//             break;
//         }
//         total_written += len;
//         ESP_LOGD(TAG, "Written %d bytes", total_written);
//     }

//     esp_http_client_close(client);
//     esp_http_client_cleanup(client);

//     if (!write_ok) {
//         esp_ota_abort(ota_handle);
//         return std::unexpected(Error::OtaWrite);
//     }

//     /* esp_ota_end() validates the image header + SHA-256 stored in the
//      * app descriptor against the downloaded data */
//     if (esp_ota_end(ota_handle) != ESP_OK) {
//         return std::unexpected(Error::Sha256Mismatch);
//     }

//     ESP_LOGI(TAG, "OTA write complete (%d bytes). Verifying signature...",
//              total_written);

//     /* ── ECDSA signature: re-map the partition and verify ─────────────── */
//     const uint8_t *flash_ptr = nullptr;
//     spi_flash_mmap_handle_t mmap_handle{};
//     if (esp_partition_mmap(target_partition, 0,
//                            static_cast<size_t>(total_written),
//                            SPI_FLASH_MMAP_DATA,
//                            reinterpret_cast<const void **>(&flash_ptr),
//                            &mmap_handle) == ESP_OK)
//     {
//         auto sig_result = verify_signature(
//             std::span<const uint8_t>(flash_ptr,
//                                      static_cast<size_t>(total_written)),
//             info.sig_b64, ecdsa_pub_pem);
//         spi_flash_munmap(mmap_handle);

//         if (!sig_result) {
//             ESP_LOGE(TAG, "Signature check failed – aborting update");
//             /* Erase the partition so it won't be booted accidentally */
//             esp_partition_erase_range(target_partition, 0,
//                                       target_partition->size);
//             return std::unexpected(Error::SignatureInvalid);
//         }
//     } else {
//         ESP_LOGW(TAG, "Could not mmap partition for sig verify – skipping");
//     }

//     return {};
// }

// } // anonymous namespace

// /* ── Public API ─────────────────────────────────────────────────────────── */

// namespace ota_manager {

// Result<FirmwareInfo> fetch_manifest(const Config &cfg,
//                                     boot_ctrl::Slot target_slot) noexcept
// {
//     auto body = https_get_string(cfg.manifest_url, cfg.ca_cert_pem,
//                                  cfg.timeout_ms);
//     if (!body) return std::unexpected(body.error());

//     return parse_manifest(body->c_str(), target_slot);
// }

// Result<void> check_and_update(const Config   &cfg,
//                                boot_ctrl::Slot target_slot) noexcept
// {
//     /* 1. Fetch + parse manifest */
//     auto info_result = fetch_manifest(cfg, target_slot);
//     if (!info_result) return std::unexpected(info_result.error());
//     const FirmwareInfo &info = *info_result;

//     /* 2. Get the installed version for the target slot.
//      *    If the target slot IS the running slot, use esp_app_get_description().
//      *    Otherwise read the app_desc from the partition header. */
//     const esp_partition_t *target_part = esp_partition_find_first(
//         ESP_PARTITION_TYPE_APP,
//         (target_slot == boot_ctrl::Slot::FirmwareA)
//             ? ESP_PARTITION_SUBTYPE_APP_OTA_0
//             : ESP_PARTITION_SUBTYPE_APP_OTA_1,
//         nullptr);

//     if (!target_part) return std::unexpected(Error::PartitionNotFound);

//     const char *installed_ver = "0.0.0";
//     esp_app_desc_t app_desc{};
//     if (esp_ota_get_partition_description(target_part, &app_desc) == ESP_OK) {
//         installed_ver = app_desc.version;
//     }

//     /* 3. Version gate */
//     if (!is_newer(info.version, installed_ver)) {
//         ESP_LOGI(TAG, "Slot %s is already up-to-date (%s)",
//                  boot_ctrl::slot_name(target_slot).data(), installed_ver);
//         return std::unexpected(Error::VersionCurrent);
//     }

//     ESP_LOGI(TAG, "Updating slot %s: %s → %s",
//              boot_ctrl::slot_name(target_slot).data(),
//              installed_ver, info.version.c_str());

//     /* 4. Safety guard: refuse to overwrite the currently running slot */
//     if (target_slot == boot_ctrl::running_slot()) {
//         ESP_LOGE(TAG, "Refusing to overwrite the running partition. "
//                       "Switch to the other slot first.");
//         return std::unexpected(Error::OtaBegin);
//     }

//     /* 5. Download, flash, verify */
//     auto flash_result = download_and_flash(
//         info, cfg.ca_cert_pem, cfg.ecdsa_pub_pem,
//         target_part, cfg.timeout_ms);

//     if (!flash_result) return flash_result;

//     ESP_LOGI(TAG, "Firmware update successful. %s",
//              cfg.reboot_after_update ? "Rebooting..." : "Reboot to apply.");

//     if (cfg.reboot_after_update) {
//         vTaskDelay(pdMS_TO_TICKS(300));
//         esp_restart();
//     }

//     return {};
// }

// } // namespace ota_manager

/*
ota_manager.cpp
HTTPS OTA with zero-heap JSON parsing (jsmn instead of cJSON)
*/
#include "ota_manager.hpp"
#include <array>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstring>
#include <span>
#include "jsmn.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_http_client.h"
#include "esp_app_desc.h"
#include <psa/crypto.h>

static const char *TAG = "ota_manager";

namespace {
constexpr size_t kDownloadBufSize = 4096;
constexpr size_t kMaxJsonTokens = 128;  // Sufficient for manifest parsing
constexpr size_t kManifestMaxSize = 2048;  // Max manifest JSON size

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

static int decode_hex_char(char c) noexcept
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::string_view strip_pem(std::string_view pem,
                                 std::string_view begin_marker,
                                 std::string_view end_marker) noexcept
{
    size_t begin = pem.find(begin_marker);
    if (begin == std::string_view::npos) {
        return {};
    }
    begin += begin_marker.size();

    size_t end = pem.find(end_marker, begin);
    if (end == std::string_view::npos) {
        return {};
    }

    std::string_view body = pem.substr(begin, end - begin);
    size_t first = 0;
    while (first < body.size() && std::isspace(static_cast<unsigned char>(body[first]))) {
        ++first;
    }
    size_t last = body.size();
    while (last > first && std::isspace(static_cast<unsigned char>(body[last - 1]))) {
        --last;
    }

    return body.substr(first, last - first);
}

static bool base64_decode(std::string_view src,
                          std::array<uint8_t, 512> &dst,
                          size_t &out_len) noexcept
{
    static const int8_t table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };

    uint32_t buffer = 0;
    int bits = 0;
    out_len = 0;
    bool saw_padding = false;

    for (char c : src) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc == '=') {
            saw_padding = true;
            continue;
        }
        if (uc == '\r' || uc == '\n' || uc == ' ' || uc == '\t') {
            continue;
        }
        if (saw_padding) {
            return false;
        }

        int8_t value = table[uc];
        if (value < 0) {
            return false;
        }

        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out_len >= dst.size()) {
                return false;
            }
            dst[out_len++] = static_cast<uint8_t>((buffer >> bits) & 0xFF);
        }
    }

    return true;
}

static bool parse_public_key_pem(std::string_view pem,
                                 std::array<uint8_t, 512> &der,
                                 size_t &der_len) noexcept
{
    const std::string_view public_key_begin = "-----BEGIN PUBLIC KEY-----";
    const std::string_view public_key_end = "-----END PUBLIC KEY-----";
    const std::string_view ec_key_begin = "-----BEGIN EC PUBLIC KEY-----";
    const std::string_view ec_key_end = "-----END EC PUBLIC KEY-----";

    std::string_view body = strip_pem(pem, public_key_begin, public_key_end);
    if (body.empty()) {
        body = strip_pem(pem, ec_key_begin, ec_key_end);
    }
    if (body.empty()) {
        return false;
    }

    return base64_decode(body, der, der_len);
}

/* ── JSMN Helper: Find JSON value by key ───────────────────────────────── */
[[nodiscard]] const char* find_json_value(
    const char *json,
    const jsmntok_t *tokens,
    int num_tokens,
    const char *key,
    size_t &out_len) noexcept
{
    for (int i = 0; i < num_tokens - 1; ++i) {
        if (tokens[i].type == JSMN_STRING) {
            // Check if this token matches the key
            if (tokens[i].size > 0 && 
                strncmp(json + tokens[i].start, key, tokens[i].end - tokens[i].start) == 0) {
                // Next token is the value
                const jsmntok_t &val = tokens[i + 1];
                if (val.type == JSMN_STRING || val.type == JSMN_PRIMITIVE) {
                    out_len = val.end - val.start;
                    return json + val.start;
                }
            }
        }
    }
    out_len = 0;
    return nullptr;
}

/* ── Version comparison ─────────────────────────────────────────────────── */
struct SemVer { uint32_t major{}, minor{}, patch{}; };

[[nodiscard]] SemVer parse_semver(std::string_view v) noexcept
{
    SemVer sv;
    auto it = v.begin();
    auto parse_num = [&](uint32_t &out) {
        auto [ptr, ec] = std::from_chars(it, v.end(), out);
        it = ptr;
        if (it != v.end() && *it == '.') ++it;
    };
    parse_num(sv.major);
    parse_num(sv.minor);
    parse_num(sv.patch);
    return sv;
}

[[nodiscard]] bool is_newer(std::string_view remote, std::string_view local) noexcept
{
    const auto r = parse_semver(remote);
    const auto l = parse_semver(local);
    if (r.major != l.major) return r.major > l.major;
    if (r.minor != l.minor) return r.minor > l.minor;
    return r.patch > l.patch;
}

/* ── JSON manifest parser (jsmn, no malloc) ────────────────────────────── */
[[nodiscard]] ota_manager::Result<ota_manager::FirmwareInfo>
parse_manifest(const char *json_str, size_t json_len, boot_ctrl::Slot target) noexcept
{
    using namespace ota_manager;
    
    jsmn_parser parser;
    jsmn_init(&parser);
    
    etl::array<jsmntok_t, kMaxJsonTokens> tokens;
    int num_tokens = jsmn_parse(&parser, json_str, json_len, 
                                 tokens.data(), tokens.size());
    
    if (num_tokens < 0) {
        ESP_LOGE(TAG, "JSMN parse error: %d", num_tokens);
        return tl::unexpected(Error::ManifestParse);
    }
    
    // Select firmware object key based on slot
    const char *fw_key = (target == boot_ctrl::Slot::FirmwareA) 
                         ? "firmware_a" : "firmware_b";
    
    // Find the firmware object
    int fw_start = -1, fw_end = -1;
    for (int i = 0; i < num_tokens - 1; ++i) {
        if (tokens[i].type == JSMN_STRING && 
            strncmp(json_str + tokens[i].start, fw_key, tokens[i].end - tokens[i].start) == 0) {
            fw_start = tokens[i + 1].start;
            fw_end = tokens[i + 1].end;
            break;
        }
    }
    
    if (fw_start < 0) {
        return tl::unexpected(Error::ManifestParse);
    }

    // Narrow the token window to only those inside the firmware object.
    // Token offsets are ABSOLUTE into json_str, so find_json_value must
    // always receive json_str as its base pointer.  We restrict the search
    // to tokens whose start falls within [fw_start, fw_end) so that keys
    // in sibling firmware objects are never matched.
    int fw_tok_first = -1;
    int fw_tok_last  = num_tokens;   // exclusive upper bound
    for (int i = 0; i < num_tokens; ++i) {
        if (fw_tok_first < 0 && tokens[i].start >= fw_start) {
            fw_tok_first = i;
        }
        if (fw_tok_first >= 0 && tokens[i].start >= fw_end) {
            fw_tok_last = i;
            break;
        }
    }
    if (fw_tok_first < 0) {
        return tl::unexpected(Error::ManifestParse);
    }

    // Extract fields from firmware object
    size_t len = 0;
    FirmwareInfo info;

    // Clear all strings first
    info.version.clear();
    info.url.clear();
    info.sha256_hex.clear();
    info.sig_b64.clear();

    const jsmntok_t *fw_tokens = tokens.data() + fw_tok_first;
    const int        fw_ntok   = fw_tok_last - fw_tok_first;

    // Parse each field — base pointer is always json_str (absolute offsets)
    const char *val = find_json_value(json_str, fw_tokens, fw_ntok,
                                      "version", len);
    if (val && len <= info.version.capacity()) {
        info.version.append(val, len);
    }

    val = find_json_value(json_str, fw_tokens, fw_ntok, "url", len);
    if (val && len <= info.url.capacity()) {
        info.url.append(val, len);
    }

    val = find_json_value(json_str, fw_tokens, fw_ntok, "sha256", len);
    if (val && len <= info.sha256_hex.capacity()) {
        info.sha256_hex.append(val, len);
    }

    val = find_json_value(json_str, fw_tokens, fw_ntok, "signature", len);
    if (val && len <= info.sig_b64.capacity()) {
        info.sig_b64.append(val, len);
    }
    
    // Validate required fields
    if (info.version.empty() || info.url.empty() ||
        info.sha256_hex.empty() || info.sig_b64.empty()) {
        return tl::unexpected(Error::ManifestParse);
    }
    
    return info;
}

/* ── HTTPS GET into fixed buffer (manifest only, small payload) ────────── */
[[nodiscard]] ota_manager::Result<etl::array<char, kManifestMaxSize>>
https_get_buffer(std::string_view url, std::string_view ca_pem,
                 uint32_t timeout_ms) noexcept
{
    using namespace ota_manager;
    
    esp_http_client_config_t cfg{};
    cfg.url = url.data();
    cfg.cert_pem = ca_pem.data();
    cfg.timeout_ms = static_cast<int>(timeout_ms);
    cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
    
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return tl::unexpected(Error::HttpFetch);
    
    etl::array<char, kManifestMaxSize> buffer{};
    size_t total_read = 0;
    
    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return tl::unexpected(Error::TlsError);
    }
    
    esp_http_client_fetch_headers(client);
    
    char chunk[512];
    int len = 0;
    while ((len = esp_http_client_read(client, chunk, sizeof(chunk))) > 0) {
        if (total_read + len >= kManifestMaxSize) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return tl::unexpected(Error::ManifestParse);  // Buffer overflow
        }
        std::memcpy(buffer.data() + total_read, chunk, len);
        total_read += len;
    }
    
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    
    buffer[total_read] = '\0';  // Null-terminate
    return buffer;
}

/* ── ECDSA-P256 signature verification (PSA) ─────────────────────────── */
[[nodiscard]] ota_manager::Result<void>
verify_signature(etl::span<const uint8_t> binary,
                 std::string_view sig_b64,
                 std::string_view pub_pem) noexcept
{
    using namespace ota_manager;

    if (!ensure_psa_initialized()) {
        return tl::unexpected(Error::SignatureInvalid);
    }

    std::array<uint8_t, 512> sig_buf{};
    size_t sig_len = 0;
    if (!base64_decode(sig_b64, sig_buf, sig_len)) {
        return tl::unexpected(Error::SignatureInvalid);
    }

    etl::array<uint8_t, 32> hash{};
    size_t hash_len = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256,
                         binary.data(), binary.size(),
                         hash.data(), hash.size(),
                         &hash_len) != PSA_SUCCESS || hash_len != hash.size()) {
        return tl::unexpected(Error::SignatureInvalid);
    }

    std::array<uint8_t, 512> pub_der{};
    size_t pub_der_len = 0;
    if (!parse_public_key_pem(pub_pem, pub_der, pub_der_len)) {
        return tl::unexpected(Error::SignatureInvalid);
    }

    // PSA expects the raw uncompressed EC point (04 || X || Y, 65 bytes), not the
    // SubjectPublicKeyInfo DER that PEM files carry.  Locate the BIT STRING that
    // wraps the point: tag=0x03, length=0x42, unused-bits=0x00, then 0x04 marker.
    const uint8_t *key_bytes = pub_der.data();
    size_t         key_bytes_len = pub_der_len;
    for (size_t i = 0; i + 4 <= pub_der_len; ++i) {
        if (pub_der[i]   == 0x03 &&   // BIT STRING tag
            pub_der[i+1] == 0x42 &&   // content length = 66
            pub_der[i+2] == 0x00 &&   // unused bits = 0
            pub_der[i+3] == 0x04) {   // uncompressed point marker
            key_bytes     = pub_der.data() + i + 3;  // 04 || X || Y
            key_bytes_len = 65;
            break;
        }
    }
    if (key_bytes_len != 65) {
        ESP_LOGE(TAG, "Could not locate EC uncompressed point in public key DER");
        return tl::unexpected(Error::SignatureInvalid);
    }

    psa_key_attributes_t attrs = psa_key_attributes_init();
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attrs, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_type(&attrs, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attrs, 256);

    mbedtls_svc_key_id_t key_id{};
    if (psa_import_key(&attrs, key_bytes, key_bytes_len, &key_id) != PSA_SUCCESS) {
        return tl::unexpected(Error::SignatureInvalid);
    }

    psa_status_t status = psa_verify_hash(key_id,
                                         PSA_ALG_ECDSA(PSA_ALG_SHA_256),
                                         hash.data(), hash_len,
                                         sig_buf.data(), sig_len);
    psa_destroy_key(key_id);

    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Signature verification failed (PSA)");
        return tl::unexpected(Error::SignatureInvalid);
    }

    return {};
}

/* ── OTA download + write loop ─────────────────────────────────────────── */
[[nodiscard]] ota_manager::Result<void>
download_and_flash(const ota_manager::FirmwareInfo &info,
                   std::string_view ca_pem,
                   std::string_view ecdsa_pub_pem,
                   const esp_partition_t *target_partition,
                   uint32_t timeout_ms) noexcept
{
    using namespace ota_manager;
    
    esp_ota_handle_t ota_handle{};
    if (esp_ota_begin(target_partition, OTA_WITH_SEQUENTIAL_WRITES,
                      &ota_handle) != ESP_OK) {
        return tl::unexpected(Error::OtaBegin);
    }
    
    // HTTP stream
    esp_http_client_config_t cfg{};
    cfg.url = info.url.c_str();
    cfg.cert_pem = ca_pem.data();
    cfg.timeout_ms = static_cast<int>(timeout_ms);
    cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
    cfg.buffer_size = kDownloadBufSize;
    
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        esp_ota_abort(ota_handle);
        return tl::unexpected(Error::HttpFetch);
    }
    
    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        esp_ota_abort(ota_handle);
        return tl::unexpected(Error::TlsError);
    }
    
    esp_http_client_fetch_headers(client);
    
    etl::array<uint8_t, kDownloadBufSize> buf{};
    int len = 0;
    bool write_ok = true;
    int total_written = 0;
    
    while ((len = esp_http_client_read(client,
                reinterpret_cast<char*>(buf.data()),
                static_cast<int>(buf.size()))) > 0) {
        if (esp_ota_write(ota_handle, buf.data(),
                          static_cast<size_t>(len)) != ESP_OK) {
            write_ok = false;
            break;
        }
        total_written += len;
        ESP_LOGD(TAG, "Written %d bytes", total_written);
    }
    
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    
    if (!write_ok) {
        esp_ota_abort(ota_handle);
        return tl::unexpected(Error::OtaWrite);
    }
    
    if (esp_ota_end(ota_handle) != ESP_OK) {
        return tl::unexpected(Error::Sha256Mismatch);
    }
    
    ESP_LOGI(TAG, "OTA write complete (%d bytes). Verifying signature...",
             total_written);
    
    // ECDSA signature verification
    const uint8_t *flash_ptr = nullptr;
    esp_partition_mmap_handle_t mmap_handle{};
    if (esp_partition_mmap(target_partition, 0,
                           static_cast<size_t>(total_written),
                           ESP_PARTITION_MMAP_DATA,
                           reinterpret_cast<const void**>(&flash_ptr),
                           &mmap_handle) == ESP_OK) {
        auto sig_result = verify_signature(
            etl::span<const uint8_t>(flash_ptr,
                                     static_cast<size_t>(total_written)),
            std::string_view(info.sig_b64.data(), info.sig_b64.size()),
            ecdsa_pub_pem);
        esp_partition_munmap(mmap_handle);
        
        if (!sig_result) {
            ESP_LOGE(TAG, "Signature check failed – aborting update");
            esp_partition_erase_range(target_partition, 0,
                                      target_partition->size);
            return tl::unexpected(Error::SignatureInvalid);
        }
    } else {
        ESP_LOGW(TAG, "Could not mmap partition for sig verify – skipping");
    }
    
    return {};
}

} // anonymous namespace

/* ── Public API ────────────────────────────────────────────────────────── */
namespace ota_manager {

Result<FirmwareInfo> fetch_manifest(const Config &cfg,
                                    boot_ctrl::Slot target_slot) noexcept
{
    auto buffer_result = https_get_buffer(cfg.manifest_url, cfg.ca_cert_pem,
                                          cfg.timeout_ms);
    if (!buffer_result) return tl::unexpected(buffer_result.error());
    
    const auto& buffer = *buffer_result;
    size_t json_len = std::strlen(buffer.data());
    return parse_manifest(buffer.data(), json_len, target_slot);
}

Result<void> check_and_update(const Config &cfg,
                              boot_ctrl::Slot target_slot) noexcept
{
    // 1. Fetch + parse manifest
    auto info_result = fetch_manifest(cfg, target_slot);
    if (!info_result) return tl::unexpected(info_result.error());
    const FirmwareInfo &info = *info_result;
    
    // 2. Get installed version for target slot
    const esp_partition_t *target_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        (target_slot == boot_ctrl::Slot::FirmwareA)
            ? ESP_PARTITION_SUBTYPE_APP_OTA_0
            : ESP_PARTITION_SUBTYPE_APP_OTA_1,
        nullptr);
    
    if (!target_part) return tl::unexpected(Error::PartitionNotFound);
    
    const char *installed_ver = "0.0.0";
    esp_app_desc_t app_desc{};
    if (esp_ota_get_partition_description(target_part, &app_desc) == ESP_OK) {
        installed_ver = app_desc.version;
    }
    
    // 3. Version gate
    if (!is_newer(std::string_view(info.version.data(), info.version.size()),
                   installed_ver)) {
        ESP_LOGI(TAG, "Slot %s is already up-to-date (%s)",
                 boot_ctrl::slot_name(target_slot).data(), installed_ver);
        return tl::unexpected(Error::VersionCurrent);
    }
    
    ESP_LOGI(TAG, "Updating slot %s: %s → %s",
             boot_ctrl::slot_name(target_slot).data(),
             installed_ver, info.version.c_str());
    
    // 4. Safety guard: refuse to overwrite running slot
    if (target_slot == boot_ctrl::running_slot()) {
        ESP_LOGE(TAG, "Refusing to overwrite the running partition.");
        return tl::unexpected(Error::OtaBegin);
    }
    
    // 5. Download, flash, verify
    auto flash_result = download_and_flash(
        info, cfg.ca_cert_pem, cfg.ecdsa_pub_pem,
        target_part, cfg.timeout_ms);
    
    if (!flash_result) return flash_result;
    
    ESP_LOGI(TAG, "Firmware update successful. %s",
             cfg.reboot_after_update ? "Rebooting..." : "Reboot to apply.");
    
    if (cfg.reboot_after_update) {
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    
    return {};
}

} // namespace ota_manager