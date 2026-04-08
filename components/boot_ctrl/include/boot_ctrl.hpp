#pragma once
/*
boot_ctrl.hpp
Manages:
• Which firmware slot is running / standby
• Requesting a runtime switch  (takes effect after reboot)
• Boot-failure counting and auto-rollback
• Marking the current boot as healthy

C++17 Compatible - No C++20/23 features
*/
#include <cstdint>
#include <string_view>
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "expected.hpp"  // expected instead of std::expected

namespace boot_ctrl {
/* ── Types ──────────────────────────────────────────────────────────────── */
enum class Slot : uint8_t {
    FirmwareA = 0,   ///< ota_0
    FirmwareB = 1,   ///< ota_1
};

enum class Error : uint8_t {
    NvsOpen,
    NvsRead,
    NvsWrite,
    PartitionNotFound,
    OtaSetBoot,
    AlreadyRunning,
};

// C++17: Use expected instead of std::expected (C++23)
template<typename T>
using Result = tl::expected<T, Error>;

/* ── Constants ──────────────────────────────────────────────────────────── */
inline constexpr uint8_t  kMaxBootFailures  = 3;
inline constexpr uint32_t kRebootDelayMs    = 300;   ///< flush logs before reset

/* ── API ────────────────────────────────────────────────────────────────── */
/**
@brief  Return which slot is currently executing.
*/
[[nodiscard]] Slot running_slot() noexcept;

/**
@brief  Return the slot that is NOT currently running.
*/
[[nodiscard]] Slot standby_slot() noexcept;

/**
@brief  Switch to @p target after a short delay and reboot.
    Returns Error::AlreadyRunning if @p target is already active.
*/
[[nodiscard]] Result<void> switch_to(Slot target) noexcept;

/**
@brief  Call this once the application has fully initialised and is
    considered healthy.  Resets the boot-failure counter and marks
    the OTA image as valid (cancels any pending rollback).
*/
[[nodiscard]] Result<void> mark_healthy() noexcept;

/**
@brief  Increment the boot-failure counter.  If it reaches
    kMaxBootFailures the device switches to the other slot.
    Call this early in app_main, BEFORE mark_healthy().
*/
[[nodiscard]] Result<void> record_boot_attempt() noexcept;

/**
@brief  Human-readable slot name for logging.
*/
[[nodiscard]] constexpr std::string_view slot_name(Slot s) noexcept
{
    return (s == Slot::FirmwareA) ? "FirmwareA (ota_0)" : "FirmwareB (ota_1)";
}

} // namespace boot_ctrl