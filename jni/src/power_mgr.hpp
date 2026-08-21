#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <lvgl.h>

#include "device_config.hpp"
#include "input_event.hpp"

namespace Leticia {

/**
 * @brief Power management states for the display.
 */
enum class power_state {
    on,        /**< Display and system fully active */
    dimmed,    /**< Backlight faded down, panel still lit */
    sleep,     /**< Display off, system idle (music can play) */
    deep_sleep /**< Deep sleep mode (reserved for future) */
};

/**
 * @brief Owns display blanking, backlight fade, touch input gating, and
 *        the inactivity/power button handling for the recovery UI. One
 *        instance is expected to live for the lifetime of the display.
 */
class power_manager final {
public:
    power_manager() = default;
    ~power_manager();

    power_manager(const power_manager &) = delete;
    power_manager &operator=(const power_manager &) = delete;

    /**
     * @brief Initialize the power management subsystem.
     * @param disp Pointer to the LVGL display object.
     * @param config Per-device quirks (backlight node, whether fb-level
     *        blanking actually works on this panel, etc), loaded at
     *        runtime by load_device_config() so the same binary works
     *        across devices without recompiling. Pass a default-constructed
     *        device_config_t to fall back to auto-detection.
     */
    void init(lv_display_t *disp, const device_config_t &config = device_config_t{});

    /**
     * @brief Register the touchscreen input device so it can be disabled
     *        while asleep. Safe to call with nullptr (e.g. no touch node
     *        found) or not at all -- touch gating is simply skipped.
     *        Only the power button (via input_event_monitor, a separate
     *        evdev node) can wake the display; touch is intentionally
     *        inert while asleep so a screen-off phone in a pocket or bag
     *        cannot register accidental taps.
     */
    void set_touch_indev(lv_indev_t *indev);

    /**
     * @brief Set the current power state.
     * @return true on success.
     */
    bool set_state(power_state state);

    /**
     * @brief Get the current power state.
     */
    power_state get_state() const;

    /**
     * @brief Toggle between on and sleep states.
     * @return The new power state.
     */
    power_state toggle_sleep();

    /**
     * @brief Set the inactivity timeout before the screen dims (in seconds).
     */
    void set_dim_timeout(uint32_t timeout_seconds);

    /**
     * @brief Set the inactivity timeout before auto-sleep (in seconds).
     * @param timeout_seconds Timeout in seconds (0 to disable auto-sleep).
     *        The screen dims briefly as a warning shortly before this fires,
     *        the same way phones and recovery UIs do.
     */
    void set_sleep_timeout(uint32_t timeout_seconds);

    /**
     * @brief Reset the inactivity timer (call on user interaction).
     */
    void reset_activity_timer();

    /**
     * @brief Enable or disable the power button handler.
     */
    void set_pwr_button_enabled(bool enabled);

    /**
     * @brief Cleanup power management resources.
     */
    void deinit();

private:
    lv_display_t *disp_ = nullptr;
    int fb_fd_ = -1;
    std::string fb_path_;
    power_state current_state_ = power_state::on;
    uint32_t sleep_timeout_sec_ = 0;
    uint32_t dim_timeout_sec_ = 0;
    uint64_t last_activity_time_ms_ = 0;
    bool pwr_button_enabled_ = true;
    bool initialized_ = false;
    lv_timer_t *activity_timer_ = nullptr;

    input_event_monitor input_monitor_;
    device_config_t device_config_;
    lv_indev_t *touch_indev_ = nullptr;

    std::string backlight_path_;
    int max_brightness_ = 255;
    int brightness_percent_ = 100;
    lv_timer_t *fade_timer_ = nullptr;
    int fade_from_percent_ = 0;
    int fade_to_percent_ = 0;
    uint64_t fade_start_ms_ = 0;
    lv_timer_t *dim_hold_timer_ = nullptr;
    std::function<void()> fade_complete_cb_;

    bool set_display_blank(bool blank);
    bool set_display_blank_sysfs(bool blank);
    bool find_backlight();
    int read_current_brightness_percent();
    void write_brightness_percent(int percent);
    void fade_brightness_to(int target_percent, std::function<void()> on_complete = nullptr);
    void finish_sleep_after_fade();
    void cancel_dim_hold();
    void on_input_event(const input_event_t &event);
    void set_touch_enabled(bool enabled);

    static void activity_timer_trampoline(lv_timer_t *timer);
    static void fade_timer_trampoline(lv_timer_t *timer);
    static void dim_hold_timer_trampoline(lv_timer_t *timer);
};

} // namespace Leticia
