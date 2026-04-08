/*
 * boot_ctrl.cpp
 *
 * Implementation of the boot-control component.
 * All NVS keys live under the "boot_ctrl" namespace.
 */

#include "boot_ctrl.hpp"

#include <cstring>
#include <utility>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "boot_ctrl";

/* ── NVS keys ───────────────────────────────────────────────────────────── */
static constexpr char kNvsNamespace[]   = "boot_ctrl";
static constexpr char kKeyBootFails[]   = "boot_fails";

namespace boot_ctrl {

/* ── Internal helpers ───────────────────────────────────────────────────── */

namespace {

/** Map a Slot enum to its esp_partition_subtype_t */
[[nodiscard]] constexpr esp_partition_subtype_t subtype_of(Slot s) noexcept
{
    return (s == Slot::FirmwareA)
        ? ESP_PARTITION_SUBTYPE_APP_OTA_0
        : ESP_PARTITION_SUBTYPE_APP_OTA_1;
}

/** Find a partition by slot, or return nullptr */
[[nodiscard]] const esp_partition_t *partition_for(Slot s) noexcept
{
    return esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, subtype_of(s), nullptr);
}

/** Derive which Slot we are from the running partition's subtype */
[[nodiscard]] Slot slot_from_partition(const esp_partition_t *p) noexcept
{
    return (p && p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0)
        ? Slot::FirmwareA
        : Slot::FirmwareB;
}

/** RAII wrapper around nvs_handle_t */
struct NvsHandle {
    nvs_handle_t h{};
    bool         open{false};

    explicit NvsHandle(nvs_open_mode_t mode) noexcept
    {
        open = (nvs_open(kNvsNamespace, mode, &h) == ESP_OK);
    }
    ~NvsHandle() { if (open) nvs_close(h); }

    // Non-copyable, movable
    NvsHandle(const NvsHandle &)            = delete;
    NvsHandle &operator=(const NvsHandle &) = delete;
};

} // anonymous namespace

/* ── Public API ─────────────────────────────────────────────────────────── */

Slot running_slot() noexcept
{
    return slot_from_partition(esp_ota_get_running_partition());
}

Slot standby_slot() noexcept
{
    const Slot r = running_slot();
    return (r == Slot::FirmwareA) ? Slot::FirmwareB : Slot::FirmwareA;
}

Result<void> switch_to(Slot target) noexcept
{
    if (target == running_slot()) {
        ESP_LOGW(TAG, "Already running %.*s – ignoring switch request",
                 static_cast<int>(slot_name(target).size()),
                 slot_name(target).data());
        return tl::unexpected(Error::AlreadyRunning);
    }

    const esp_partition_t *dest = partition_for(target);
    if (dest == nullptr) {
        ESP_LOGE(TAG, "Partition for %.*s not found",
                 static_cast<int>(slot_name(target).size()),
                 slot_name(target).data());
        return tl::unexpected(Error::PartitionNotFound);
    }

    ESP_LOGI(TAG, "Scheduling switch → %.*s on next boot",
             static_cast<int>(slot_name(target).size()),
             slot_name(target).data());

    if (esp_ota_set_boot_partition(dest) != ESP_OK) {
        return tl::unexpected(Error::OtaSetBoot);
    }

    /* Brief delay so any pending log writes or network flushes can complete */
    vTaskDelay(pdMS_TO_TICKS(kRebootDelayMs));
    esp_restart();

    /* Unreachable, but satisfies [[noreturn]]-free signature */
    return {};
}

Result<void> mark_healthy() noexcept
{
    NvsHandle nvs{NVS_READWRITE};
    if (!nvs.open) return tl::unexpected(Error::NvsOpen);

    if (nvs_set_u8(nvs.h, kKeyBootFails, 0) != ESP_OK ||
        nvs_commit(nvs.h)                   != ESP_OK)
    {
        return tl::unexpected(Error::NvsWrite);
    }

    /* Tell the bootloader this image is good; cancel any rollback timer */
    esp_ota_mark_app_valid_cancel_rollback();

    ESP_LOGI(TAG, "Boot marked healthy for %.*s",
             static_cast<int>(slot_name(running_slot()).size()),
             slot_name(running_slot()).data());
    return {};
}

Result<void> record_boot_attempt() noexcept
{
    NvsHandle nvs{NVS_READWRITE};
    if (!nvs.open) return tl::unexpected(Error::NvsOpen);

    uint8_t fails = 0;
    /* Ignore "not found" – first boot will naturally return 0 */
    nvs_get_u8(nvs.h, kKeyBootFails, &fails);
    ++fails;

    ESP_LOGD(TAG, "Boot attempts for current slot: %u / %u",
             fails, kMaxBootFailures);

    if (nvs_set_u8(nvs.h, kKeyBootFails, fails) != ESP_OK ||
        nvs_commit(nvs.h)                        != ESP_OK)
    {
        return tl::unexpected(Error::NvsWrite);
    }

    if (fails >= kMaxBootFailures) {
        const Slot other = standby_slot();
        ESP_LOGW(TAG, "Too many boot failures – switching to %.*s",
                 static_cast<int>(slot_name(other).size()),
                 slot_name(other).data());

        /* Reset counter so the other slot gets a fresh chance */
        nvs_set_u8(nvs.h, kKeyBootFails, 0);
        nvs_commit(nvs.h);

        /* switch_to() will reboot; error here is non-recoverable */
        (void)switch_to(other);
    }

    return {};
}

} // namespace boot_ctrl