#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <lvgl.h>

#include "config/device_config.hpp"
#include "input/input_event.hpp"

namespace Leticia {

    enum class power_state {
        on,        /**< Active state. */
        dimmed,    /**< Dimmed state. */
        sleep,     /**< Sleep state. */
        deep_sleep /**< Deep sleep state. */
    };

/**
 * @brief Manages display power and backlight.
 */
    class power_manager final {
    public:
        power_manager() = default;
        ~power_manager();

        power_manager(const power_manager &) = delete;
        power_manager &operator=(const power_manager &) = delete;

        /**
         * @brief Initializes the power manager.
         *
         * @param disp LVGL display pointer.
         * @param config Device configuration.
         */
        void init(lv_display_t *disp, const device_config_t &config = device_config_t{});

        /**
         * @brief Sets the touchscreen input device.
         *
         * @param indev LVGL input device pointer.
         */
        void set_touch_indev(lv_indev_t *indev);

        /**
         * @brief Sets the power state.
         *
         * @param state Target power state.
         * @return true if successful, false otherwise.
         */
        bool set_state(power_state state);

        /**
         * @brief Gets the current power state.
         *
         * @return Current power state.
         */
        power_state get_state() const;

        /**
         * @brief Toggles between on and sleep states.
         *
         * @return New power state.
         */
        power_state toggle_sleep();

        /**
         * @brief Sets the dim timeout.
         *
         * @param timeout_seconds Timeout in seconds.
         */
        void set_dim_timeout(uint32_t timeout_seconds);

        /**
         * @brief Sets the sleep timeout.
         *
         * @param timeout_seconds Timeout in seconds.
         */
        void set_sleep_timeout(uint32_t timeout_seconds);

        /**
         * @brief Resets the activity timer.
         */
        void reset_activity_timer();

        /**
         * @brief Enables or disables the power button.
         *
         * @param enabled true to enable, false to disable.
         */
        void set_pwr_button_enabled(bool enabled);

        /**
         * @brief Deinitializes the power manager.
         */
        void deinit();

        /**
         * @brief Services any pending wake transition.
         *
         * @return true if a wake was processed, false otherwise.
         */
        bool service_pending_wake();

    private:
        lv_display_t *disp_ = nullptr;
        int fb_fd_ = -1;
        std::string fb_path_;
        power_state current_state_ = power_state::on;
        uint32_t sleep_timeout_sec_ = 0;
        uint32_t dim_timeout_sec_ = 0;
        uint64_t last_activity_time_ms_ = 0;
        bool pwr_button_enabled_ = true;
        bool power_pressed_ = false;
        uint64_t power_press_start_ms_ = 0;
        bool initialized_ = false;
        bool pending_full_redraw_ = false;
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
        void blank_framebuffer_pixels();
        bool find_backlight();
        int read_current_brightness_percent();
        void write_brightness_percent(int percent);
        void fade_brightness_to(int target_percent, std::function<void()> on_complete = nullptr);
        void finish_sleep_after_fade();
        void cancel_dim_hold();
        void invalidate_all_layers();
        void on_input_event(const input_event_t &event);
        void set_touch_enabled(bool enabled);

        static void activity_timer_trampoline(lv_timer_t *timer);
        static void fade_timer_trampoline(lv_timer_t *timer);
        static void dim_hold_timer_trampoline(lv_timer_t *timer);
    };

} // namespace Leticia
