#include "gui/components/status_bar.hpp"

#include "gui/dsl.hpp"
#include "gui/ui_scale.hpp"
#include "power/power_manager.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace Leticia::gui {

namespace {

using namespace Leticia::units;

constexpr uint32_t kClockPollIntervalMs = 1000;
constexpr sp kTextSize{12.7f};
constexpr font_manager::weight kTextWeight = font_manager::weight::medium;
constexpr int kBaseMarginDp = 8;

/**
 * @brief Extra left/right dp margin to keep the edge-anchored labels clear
 * of rounded screen corners and any camera cutout that intrudes into the
 * bar from that side.
 *
 * Rounded corners are treated conservatively: the full corner radius is
 * added as horizontal clearance, since content anchored flush to the top
 * edge sits exactly where a corner's arc excludes the most horizontal
 * space. A center cutout doesn't push edge-anchored labels by itself; it
 * only matters to a layout that centers something in the bar, which this
 * one doesn't.
 */
struct safe_margins {
    int left_dp;
    int right_dp;
};

safe_margins compute_safe_margins(const Leticia::device_config_t &device_config) {
    safe_margins margins{kBaseMarginDp, kBaseMarginDp};

    margins.left_dp += device_config.screen_corner_radius_dp;
    margins.right_dp += device_config.screen_corner_radius_dp;

    if (device_config.camera_cutout == Leticia::cutout_position::top_left) {
        margins.left_dp += device_config.camera_cutout_width_dp;
    } else if (device_config.camera_cutout == Leticia::cutout_position::top_right) {
        margins.right_dp += device_config.camera_cutout_width_dp;
    }

    return margins;
}

/**
 * @brief Picks the battery glyph matching the given charge state.
 */
const char *battery_symbol(int percent, Leticia::battery_status status) {
    if (status == Leticia::battery_status::charging || status == Leticia::battery_status::full)
        return LV_SYMBOL_CHARGE;

    if (percent > 87)
        return LV_SYMBOL_BATTERY_FULL;
    if (percent > 62)
        return LV_SYMBOL_BATTERY_3;
    if (percent > 37)
        return LV_SYMBOL_BATTERY_2;
    if (percent > 12)
        return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

/**
 * @brief Wakes the display and resets the idle timer on any input activity.
 */
void status_bar_activity_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_KEY && code != LV_EVENT_ROTARY)
        return;

    auto *power = static_cast<Leticia::power_manager *>(lv_event_get_user_data(e));
    if (power == nullptr)
        return;

    Leticia::power_state state = power->get_state();
    if (state == Leticia::power_state::sleep || state == Leticia::power_state::dimmed)
        power->set_state(Leticia::power_state::on);

    power->reset_activity_timer();
}

} // namespace

status_bar::~status_bar() {
    deinit();
}

void status_bar::init(Leticia::battery_monitor &battery, Leticia::power_manager &power,
                       const Leticia::user_config_t &user_config,
                       const Leticia::device_config_t &device_config) {
    if (!user_config.timezone.empty()) {
        setenv("TZ", user_config.timezone.c_str(), 1);
        tzset();
    }

    battery_ = &battery;
    power_ = &power;
    dp height{static_cast<float>(device_config.status_bar_height_dp)};
    height_px_ = height.px();

    Leticia::ui::widget bar_widget(lv_obj_create(lv_layer_top()));
    bar_widget.width_pct(100)
            .height(height)
            .align(LV_ALIGN_TOP_MID)
            .bg_color(lv_color_black())
            .bg_opa(LV_OPA_COVER)
            .radius(0_dp)
            .pad(0_dp)
            .no_scroll()
            .on(LV_EVENT_ALL, status_bar_activity_event_cb, power_);
    lv_obj_set_style_border_width(bar_widget.raw(), 0, LV_PART_MAIN);
    bar_ = bar_widget.raw();

    safe_margins margins = compute_safe_margins(device_config);
    dp left_margin{static_cast<float>(margins.left_dp)};
    dp right_margin{static_cast<float>(margins.right_dp)};

    Leticia::ui::label time_lbl(bar_, "");
    time_lbl.font(kTextSize, kTextWeight).text_color(lv_color_white()).align(LV_ALIGN_LEFT_MID, left_margin, 0_dp);
    time_label_ = time_lbl.raw();

    Leticia::ui::label battery_lbl(bar_, "");
    battery_lbl.font(kTextSize, kTextWeight).text_color(lv_color_white()).align(LV_ALIGN_RIGHT_MID, -right_margin, 0_dp);
    battery_label_ = battery_lbl.raw();

    refresh_clock();
    refresh_battery();

    battery_->on_change([this]() { refresh_battery(); });

    clock_timer_ = lv_timer_create(clock_timer_trampoline, kClockPollIntervalMs, this);
}

void status_bar::deinit() {
    if (clock_timer_ != nullptr) {
        lv_timer_delete(clock_timer_);
        clock_timer_ = nullptr;
    }

    if (bar_ != nullptr) {
        lv_obj_delete(bar_);
        bar_ = nullptr;
        time_label_ = nullptr;
        battery_label_ = nullptr;
    }

    battery_ = nullptr;
    power_ = nullptr;
}

void status_bar::refresh_clock() {
    time_t now = time(nullptr);
    struct tm local_tm;
    localtime_r(&now, &local_tm);

    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", local_tm.tm_hour, local_tm.tm_min);
    lv_label_set_text(time_label_, buf);
}

void status_bar::refresh_battery() {
    if (battery_ == nullptr || battery_label_ == nullptr)
        return;

    if (!battery_->is_available()) {
        lv_label_set_text(battery_label_, "");
        return;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%s %d%%", battery_symbol(battery_->percent(), battery_->status()), battery_->percent());
    lv_label_set_text(battery_label_, buf);
}

void status_bar::clock_timer_trampoline(lv_timer_t *timer) {
    auto *self = static_cast<status_bar *>(lv_timer_get_user_data(timer));
    self->refresh_clock();
}

} // namespace Leticia::gui
