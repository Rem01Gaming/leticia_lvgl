#pragma once

#include <cstdint>

#include <lvgl.h>

#include "config/device_config.hpp"
#include "config/user_config.hpp"
#include "power/battery_monitor.hpp"

namespace Leticia::gui {

/**
 * @brief Persistent top status bar showing the clock and battery state.
 *
 * Built once on LVGL's top layer, so it survives every subsequent screen
 * load without being recreated. Callers reserve height_px() at the top of
 * their own screens so content never appears underneath it.
 */
class status_bar final {
public:
    status_bar() = default;
    ~status_bar();

    status_bar(const status_bar &) = delete;
    status_bar &operator=(const status_bar &) = delete;

    /**
     * @brief Builds the bar on the top layer and starts its clock/battery updates.
     *
     * @param battery Battery monitor providing charge updates for the lifetime of the bar.
     * @param user_config User configuration, used to set the display timezone.
     * @param device_config Device configuration; supplies the bar height and the
     * screen corner radius / camera cutout used to keep the clock and battery
     * labels clear of rounded corners and any cutout on their side of the bar.
     */
    void init(Leticia::battery_monitor &battery, const Leticia::user_config_t &user_config,
              const Leticia::device_config_t &device_config);

    /**
     * @brief Deinitializes the bar and stops its timer.
     */
    void deinit();

    /**
     * @brief Gets the bar's height in pixels, for views to offset their content.
     *
     * @return Height in pixels, or 0 if not initialized.
     */
    int32_t height_px() const { return height_px_; }

private:
    lv_obj_t *bar_ = nullptr;
    lv_obj_t *time_label_ = nullptr;
    lv_obj_t *battery_label_ = nullptr;
    lv_timer_t *clock_timer_ = nullptr;
    Leticia::battery_monitor *battery_ = nullptr;
    int32_t height_px_ = 0;

    void refresh_clock();
    void refresh_battery();

    static void clock_timer_trampoline(lv_timer_t *timer);
};

} // namespace Leticia::gui
