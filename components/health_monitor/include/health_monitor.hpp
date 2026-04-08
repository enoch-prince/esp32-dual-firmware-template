#pragma once

/*
 * health_monitor.hpp
 *
 * Periodically checks user-registered health probes.  If the aggregate
 * health score falls below a threshold the device automatically switches
 * to the standby firmware slot.
 *
 * Usage
 * ─────
 *   1. Register one or more probes at startup.
 *   2. Call start() to begin periodic evaluation.
 *   3. Once the application is fully healthy, call boot_ctrl::mark_healthy().
 *
 * A probe is any callable matching:  bool(void)
 *   return true  → healthy
 *   return false → fault detected
 */

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace health_monitor {

/* ── Types ───────────────────────────────────────────────────────────────── */

using Probe = std::function<bool()>;

struct ProbeEntry {
    std::string_view name;
    Probe            probe;
    uint8_t          weight{1};  ///< relative importance (1 = normal)
};

struct Config {
    uint32_t check_interval_ms{5'000};   ///< how often to run all probes
    uint8_t  failure_threshold{3};       ///< consecutive all-fail rounds before switching
    bool     auto_switch_on_failure{true};
};

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief  Register a health probe.  Call before start().
 */
void register_probe(ProbeEntry &&entry);

/**
 * @brief  Start the health-monitor task.
 */
void start(const Config &cfg);

/**
 * @brief  Stop the health-monitor task.
 */
void stop();

/**
 * @brief  Query the current consecutive-failure count.
 */
[[nodiscard]] uint8_t failure_count() noexcept;

} // namespace health_monitor
