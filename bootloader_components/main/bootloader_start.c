/*
 * bootloader_start.c
 *
 * Custom second-stage bootloader for ESP32-S2 dual-firmware system.
 *
 * Behaviour
 * ─────────
 *  • Normal boot  → reads otadata, boots whichever OTA slot is marked active.
 *  • GPIO held LOW at reset → forces boot into ota_1 (Firmware B), regardless
 *    of what otadata says.  Useful when Firmware A is broken at the app level.
 *
 * NOTE: The bootloader runs before FreeRTOS and the full ESP-IDF stack.
 *       Only ROM functions and bootloader_support APIs are available here.
 */

#include <stdbool.h>
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"
// #include "bootloader_hooks.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "hal/gpio_types.h"
#include "hal/gpio_hal.h"

/* ── Configuration ─────────────────────────────────────────────────────────── */

/** GPIO to sample at boot.  Pull it LOW to force Firmware B (ota_1).
 *  On the ESP32-S2-Mini-2 the BOOT/IO0 button is a convenient choice. */
#define FW_SELECT_GPIO      GPIO_NUM_0

/** How many microseconds to wait for the pin to settle after enabling
 *  the internal pull-up. */
#define GPIO_SETTLE_US      50U

static const char *TAG = "bootloader";

/* ── Helpers ───────────────────────────────────────────────────────────────── */

/**
 * @brief Sample the firmware-select GPIO.
 * @return true  if the pin is held LOW (force Firmware B)
 * @return false for normal boot (let otadata decide)
 */
static bool fw_select_pin_active(void)
{
    /* Enable pad as input with internal pull-up */
    esp_rom_gpio_pad_select_gpio(FW_SELECT_GPIO);
    esp_rom_gpio_pad_pullup_only(FW_SELECT_GPIO);
    esp_rom_delay_us(GPIO_SETTLE_US);
    return (gpio_ll_get_level(GPIO_HAL_GET_HW(FW_SELECT_GPIO), FW_SELECT_GPIO) == 0);
}

/* ── Entry point ───────────────────────────────────────────────────────────── */

/*
 * call_start_cpu0() is the real entry point invoked by the ROM after it
 * verifies and loads the second-stage bootloader.  We override it here.
 */
void __attribute__((noreturn)) call_start_cpu0(void)
{
    /* Mandatory bootloader initialisation (clocks, flash, etc.) */
    if (bootloader_init() != ESP_OK) {
        ESP_LOGE(TAG, "bootloader_init failed – halting");
        while (true) { /* hang */ }
    }

    /* ── GPIO override check ─────────────────────────────────────────────── */
    if (fw_select_pin_active()) {
        ESP_LOGW(TAG, "FW-select pin LOW → forcing Firmware B (ota_1)");

        /*
         * Build a boot_data structure that points directly at ota_1.
         * bootloader_utility_get_selected_boot_partition() normally fills
         * this from otadata; we bypass it by setting the index manually.
         */
        bootloader_state_t bs = {};
        if (bootloader_utility_load_partition_table(&bs)) {
            /* ota_1 is OTA index 1 */
            bootloader_utility_load_boot_image(&bs, 1);
            /* load_boot_image() does not return on success */
        }
        ESP_LOGE(TAG, "Failed to load ota_1 – falling through to normal boot");
    }

    /* ── Normal boot path (reads otadata) ───────────────────────────────── */
    bootloader_state_t bs = {};
    if (!bootloader_utility_load_partition_table(&bs)) {
        ESP_LOGE(TAG, "Failed to load partition table – halting");
        while (true) { /* hang */ }
    }

    int boot_index = bootloader_utility_get_selected_boot_partition(&bs);
    if (boot_index == INVALID_INDEX) {
        ESP_LOGE(TAG, "No valid OTA partition found – halting");
        while (true) { /* hang */ }
    }

    ESP_LOGI(TAG, "Booting OTA slot %d", boot_index);
    bootloader_utility_load_boot_image(&bs, boot_index);

    /* Should never reach here */
    while (true) {}
}
