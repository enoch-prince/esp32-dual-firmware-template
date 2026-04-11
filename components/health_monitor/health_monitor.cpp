/*
 * health_monitor.cpp
 *
 * Periodic health evaluation task.  Runs each registered probe on a
 * FreeRTOS timer and counts consecutive fully-failed rounds.  If the
 * failure counter hits the configured threshold, it delegates to
 * boot_ctrl::switch_to() for an automatic slot switch.
 */

#include "health_monitor.hpp"
#include "boot_ctrl.hpp"

#include <atomic>
#include <numeric>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "health_mon";

namespace {

/* ── State ───────────────────────────────────────────────────────────────── */

etl::vector<health_monitor::ProbeEntry, 16> g_probes;
health_monitor::Config                      g_cfg;
TaskHandle_t                                g_task{nullptr};
std::atomic<uint8_t>                        g_fail_count{0};

/* ── Monitor task ────────────────────────────────────────────────────────── */

void monitor_task(void * /*arg*/)
{
    ESP_LOGI(TAG, "Health monitor started (interval=%lu ms, threshold=%u)",
             g_cfg.check_interval_ms, g_cfg.failure_threshold);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(g_cfg.check_interval_ms));

        if (g_probes.empty()) continue;

        /* Run all probes and accumulate weighted pass/fail */
        uint32_t total_weight  = 0;
        uint32_t passed_weight = 0;

        for (const auto &entry : g_probes) {
            const bool ok = entry.probe();
            total_weight  += entry.weight;
            if (ok) {
                passed_weight += entry.weight;
            } else {
                ESP_LOGW(TAG, "Probe [%.*s] FAILED",
                         static_cast<int>(entry.name.size()),
                         entry.name.data());
            }
        }

        const bool round_healthy = (passed_weight == total_weight);

        if (round_healthy) {
            if (g_fail_count.load() > 0) {
                ESP_LOGI(TAG, "Health restored");
            }
            g_fail_count.store(0);
        } else {
            const uint8_t fails = g_fail_count.fetch_add(1) + 1;
            ESP_LOGW(TAG, "Health check failed round %u / %u  "
                          "(passed %lu / %lu weight)",
                     fails, g_cfg.failure_threshold,
                     passed_weight, total_weight);

            if (g_cfg.auto_switch_on_failure &&
                fails >= g_cfg.failure_threshold)
            {
                ESP_LOGE(TAG, "Failure threshold reached – "
                              "switching to standby firmware");
                g_fail_count.store(0);
                /* switch_to() reboots; task will not continue past this */
                (void)boot_ctrl::switch_to(boot_ctrl::standby_slot());
            }
        }
    }
}

} // anonymous namespace

/* ── Public API ─────────────────────────────────────────────────────────── */

namespace health_monitor {

void register_probe(ProbeEntry &&entry)
{
    ESP_LOGI(TAG, "Registering probe [%.*s] weight=%u",
             static_cast<int>(entry.name.size()),
             entry.name.data(), entry.weight);
    g_probes.push_back(std::move(entry));
}

void start(const Config &cfg)
{
    g_cfg = cfg;
    g_fail_count.store(0);

    xTaskCreate(
        monitor_task,
        "health_mon",
        4096,
        nullptr,
        tskIDLE_PRIORITY + 2,
        &g_task);
}

void stop()
{
    if (g_task) {
        vTaskDelete(g_task);
        g_task = nullptr;
    }
}

[[nodiscard]] uint8_t failure_count() noexcept
{
    return g_fail_count.load();
}

} // namespace health_monitor
