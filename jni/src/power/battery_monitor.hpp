#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <lvgl.h>

#include "config/device_config.hpp"

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
 * @brief Polls the battery power_supply node and reports charge changes.
 */
class battery_monitor final {
public:
    battery_monitor() = default;
    ~battery_monitor();

    battery_monitor(const battery_monitor &) = delete;
    battery_monitor &operator=(const battery_monitor &) = delete;

    /**
     * @brief Starts polling. Uses the configured path or auto-detects one.
     *
     * @param config Device configuration, checked first for battery_path.
     * @param poll_interval_ms Interval between sysfs reads, in milliseconds.
     * @return true if a battery node was found, false otherwise (device may lack a battery).
     */
    bool init(const device_config_t &config = device_config_t{}, uint32_t poll_interval_ms = 1000);

    /**
     * @brief Stops polling and releases the timer.
     */
    void deinit();

    /**
     * @brief Checks if a battery node was found.
     *
     * @return true if available, false otherwise.
     */
    bool is_available() const { return !node_path_.empty(); }

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
     * Runs on the LVGL thread, from inside lv_timer_handler().
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

    bool find_node(const device_config_t &config);
    void poll();

    static void poll_timer_trampoline(lv_timer_t *timer);
};

} // namespace Leticia
