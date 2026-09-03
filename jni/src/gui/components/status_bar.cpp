#include "gui/components/status_bar.hpp"

#include "gui/dsl.hpp"
#include "gui/ui_scale.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace Leticia::gui {

namespace {

using namespace Leticia::units;

constexpr uint32_t kClockPollIntervalMs = 1000;
constexpr sp kTextSize{14.0f};

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

} // namespace

status_bar::~status_bar() {
    deinit();
}

void status_bar::init(Leticia::battery_monitor &battery, const Leticia::user_config_t &user_config, int height_dp) {
    if (!user_config.timezone.empty()) {
        setenv("TZ", user_config.timezone.c_str(), 1);
        tzset();
    }

    battery_ = &battery;
    height_px_ = dp(static_cast<float>(height_dp)).px();

    Leticia::ui::widget bar_widget(lv_obj_create(lv_layer_top()));
    bar_widget.width_pct(100)
            .height(dp(static_cast<float>(height_dp)))
            .align(LV_ALIGN_TOP_MID)
            .bg_color(lv_color_black())
            .bg_opa(LV_OPA_COVER)
            .pad(0_dp)
            .no_scroll();
    lv_obj_set_style_border_width(bar_widget.raw(), 0, LV_PART_MAIN);
    bar_ = bar_widget.raw();

    Leticia::ui::label time_lbl(bar_, "");
    time_lbl.font(kTextSize).text_color(lv_color_white()).align(LV_ALIGN_LEFT_MID, 8_dp, 0_dp);
    time_label_ = time_lbl.raw();

    Leticia::ui::label battery_lbl(bar_, "");
    battery_lbl.font(kTextSize).text_color(lv_color_white()).align(LV_ALIGN_RIGHT_MID, -8_dp, 0_dp);
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
