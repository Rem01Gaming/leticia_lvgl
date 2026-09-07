#include "gui/components/status_bar.hpp"

#include "gui/components/battery_icons.hpp"
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
constexpr dp kBatteryIconSize{20.0f};
/* The bolt icon sits to the left of the battery icon when charging. */
constexpr dp kBoltIconSize{10.0f};
constexpr dp kBoltBatteryGap{0.2f};
constexpr dp kBatteryTextGap{4.0f};

/**
 * @brief Sets an lv_image's source to an SVG path, prefixed for LVGL's FS driver.
 *
 * @param img Image object to update.
 * @param svg_path Absolute on-disk path resolved by battery_icons, or
 * empty (in which case the image is left with no source and hidden).
 */
void set_svg_source(lv_obj_t *img, const std::string &svg_path) {
    if (svg_path.empty()) {
        lv_obj_set_hidden(img, true);
        return;
    }

    std::string fs_path = "A:" + svg_path;
    lv_image_set_src(img, fs_path.c_str());
    lv_obj_set_hidden(img, false);
}

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

    /* A row container holds the icon and percent label side by side so
     * LVGL's flex layout handles their spacing; the label's width isn't
     * known ahead of time, so a fixed offset from the bar's edge would
     * drift as the percent text changes ("5%" vs "100%"). */
    Leticia::ui::widget battery_row(lv_obj_create(bar_));
    battery_row.size_content()
            .align(LV_ALIGN_RIGHT_MID, -right_margin, 0_dp)
            .bg_opa(LV_OPA_TRANSP)
            .pad(0_dp)
            .no_scroll()
            .flex_flow(LV_FLEX_FLOW_ROW)
            .flex_align(LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(battery_row.raw(), 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(battery_row.raw(), kBoltBatteryGap.px(), LV_PART_MAIN);

    /* The SVGs are pre-colored white at the file level (fill="#FFFFFF"):
     * LVGL's SVG images render through a custom vector draw path that
     * does not apply the image_recolor style, so recoloring here would
     * have no effect. CONTAIN scales the SVG's own 960x480 viewBox down
     * to the icon's box while preserving aspect ratio; a plain size()
     * without an explicit scale/align left the 40:1 viewBox-to-declared-
     * size ratio for the renderer to resolve on its own, which silently
     * produced no output at all instead of a scaled image. */
    Leticia::ui::widget battery_bolt(lv_image_create(battery_row.raw()));
    battery_bolt.size(kBoltIconSize, kBoltIconSize).hidden(true);
    battery_bolt_ = battery_bolt.raw();
    lv_image_set_antialias(battery_bolt_, true);
    lv_image_set_inner_align(battery_bolt_, LV_IMAGE_ALIGN_CONTAIN);

    Leticia::ui::widget battery_icon(lv_image_create(battery_row.raw()));
    battery_icon.size(kBatteryIconSize, kBatteryIconSize);
    battery_icon_ = battery_icon.raw();
    lv_image_set_antialias(battery_icon_, true);
    lv_image_set_inner_align(battery_icon_, LV_IMAGE_ALIGN_CONTAIN);

    Leticia::ui::label battery_pct_lbl(battery_row.raw(), "");
    battery_pct_lbl.font(kTextSize, kTextWeight).text_color(lv_color_white());
    battery_pct_label_ = battery_pct_lbl.raw();
    lv_obj_set_style_pad_left(battery_pct_label_, kBatteryTextGap.px(), LV_PART_MAIN);

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
        battery_icon_ = nullptr;
        battery_bolt_ = nullptr;
        battery_pct_label_ = nullptr;
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
    if (battery_ == nullptr || battery_icon_ == nullptr)
        return;

    if (!battery_->is_available()) {
        lv_obj_set_hidden(battery_icon_, true);
        lv_obj_set_hidden(battery_bolt_, true);
        lv_label_set_text(battery_pct_label_, "");
        return;
    }

    int percent = battery_->percent();
    Leticia::battery_status status = battery_->status();

    lv_obj_set_hidden(battery_icon_, false);
    set_svg_source(battery_icon_, Leticia::gui::battery_icons::icon_path(percent, status));

    bool charging = (status == Leticia::battery_status::charging);
    if (charging) {
        set_svg_source(battery_bolt_, Leticia::gui::battery_icons::bolt_path());
    } else {
        lv_obj_set_hidden(battery_bolt_, true);
    }

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(battery_pct_label_, buf);
}

void status_bar::clock_timer_trampoline(lv_timer_t *timer) {
    auto *self = static_cast<status_bar *>(lv_timer_get_user_data(timer));
    self->refresh_clock();
}

} // namespace Leticia::gui
