#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <lvgl.h>

#include "config/device_config.hpp"
#include "power/battery_uevent_listener.hpp"

namespace Leticia {

/**
 * @brief Charge state read from the power supply sysfs node.
 */
enum class battery_status {
    unknown,     /**< Node missing or unreadable. */
    discharging, /**< Running on battery. */
    charging,    /**< Plugged in and charging. */
    full         /**< Plugged in, charge complete. */
};

/**
 * @brief Monitors the battery power_supply node and reports charge changes.
 *
 * Uses a netlink uevent listener for near-instant updates when the
 * process has root. A slow polling timer always runs alongside it as
 * a fallback, in case a uevent is missed or unsupported on a given kernel.
 */
class battery_monitor final {
public:
    battery_monitor() = default;
    ~battery_monitor();

    battery_monitor(const battery_monitor &) = delete;
    battery_monitor &operator=(const battery_monitor &) = delete;

    /**
     * @brief Starts monitoring. Uses the configured path or auto-detects one.
     *
     * @param config Device configuration, checked first for battery_path.
     * @param fallback_poll_interval_ms Interval for the fallback poll timer,
     *        in milliseconds. Only used as a backstop when uevents are
     *        unavailable or silently dropped by the kernel.
     * @return true if a battery node was found, false otherwise (device may lack a battery).
     */
    bool init(const device_config_t &config = device_config_t{}, uint32_t fallback_poll_interval_ms = 30000);

    /**
     * @brief Stops monitoring and releases all resources.
     */
    void deinit();

    /**
     * @brief Checks if a battery node was found.
     *
     * @return true if available, false otherwise.
     */
    bool is_available() const { return !node_path_.empty(); }

    /**
     * @brief Checks if event-driven monitoring is active.
     *
     * @return true if the uevent listener started, false if falling
     *         back to polling only (e.g. not running as root).
     */
    bool is_event_driven() const { return uevent_listener_.is_running(); }

    /**
     * @brief Gets the last read charge percentage.
     *
     * @return Percentage from 0 to 100.
     */
    int percent() const { return percent_; }

    /**
     * @brief Gets the last read charge status.
     *
     * @return Current battery status.
     */
    battery_status status() const { return status_; }

    /**
     * @brief Registers a callback fired whenever percent() or status() changes.
     *
     * Always runs on the LVGL thread, from inside lv_timer_handler(),
     * regardless of whether the update was triggered by a uevent or the
     * fallback timer.
     *
     * @param cb Callback to invoke on change.
     */
    void on_change(std::function<void()> cb) { on_change_ = std::move(cb); }

private:
    std::string node_path_;
    int percent_ = 0;
    battery_status status_ = battery_status::unknown;
    lv_timer_t *poll_timer_ = nullptr;
    std::function<void()> on_change_;
    battery_uevent_listener uevent_listener_;

    bool find_node(const device_config_t &config);
    void poll();

    static void poll_timer_trampoline(lv_timer_t *timer);
    static void async_poll_trampoline(void *self);
};

} // namespace Leticia
