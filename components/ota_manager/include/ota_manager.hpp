#pragma once
/*
ota_manager.hpp
Pulls firmware updates from an HTTPS manifest server.

C++17 Compatible - Uses expected and ETL strings
*/
#include <cstdint>
#include <string_view>
#include "expected.hpp"
#include "etl/span.h"
#include "etl/string.h"
#include "etl/array.h"
#include "boot_ctrl.hpp"

namespace ota_manager {
/* ── Error codes ────────────────────────────────────────────────────────── */
enum class Error : uint8_t {
    HttpFetch,
    ManifestParse,
    VersionCurrent,      ///< already up-to-date, not a fatal error
    SignatureInvalid,
    Sha256Mismatch,
    OtaBegin,
    OtaWrite,
    OtaEnd,
    PartitionNotFound,
    TlsError,
};

// C++17: Use expected instead of std::expected (C++23)
template<typename T>
using Result = tl::expected<T, Error>;

/* ── Manifest ───────────────────────────────────────────────────────────── */
struct FirmwareInfo {
    // C++17: Use ETL fixed strings instead of std::string to avoid heap alloc
    etl::string_fixed<32> version;      ///< semver string, e.g. "2.1.0"
    etl::string_fixed<256> url;          ///< HTTPS URL of the binary
    etl::string_fixed<64> sha256_hex;   ///< lowercase hex SHA-256 of the binary
    etl::string_fixed<128> sig_b64;      ///< base64-encoded ECDSA-P256 signature
    uint32_t    min_hw_ver{0};
};

/* ── Configuration ──────────────────────────────────────────────────────── */
struct Config {
    std::string_view manifest_url;  ///< e.g. "https://ota.example.com/manifest.json"
    std::string_view ca_cert_pem;   ///< PEM of your server's root CA (pinned)
    std::string_view ecdsa_pub_pem; ///< PEM of your signing public key
    uint32_t         timeout_ms{10'000};
    bool             reboot_after_update{false};
};

/* ── API ────────────────────────────────────────────────────────────────── */
/**
@brief  Check the manifest and update @p target_slot if a newer
    version is available.
@note   target_slot should NOT be the currently running slot unless
    you have ensured the flash-cache safety requirements are met.
    Best practice: update the standby slot.
*/
[[nodiscard]] Result<void> check_and_update(
    const Config          &cfg,
    boot_ctrl::Slot        target_slot) noexcept;

/**
@brief  Fetch and parse the manifest only (no download/write).
    Useful for displaying "update available" UI before committing.
*/
[[nodiscard]] Result<FirmwareInfo> fetch_manifest(
    const Config &cfg,
    boot_ctrl::Slot target_slot) noexcept;

} // namespace ota_manager