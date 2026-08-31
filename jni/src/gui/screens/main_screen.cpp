#include "gui/screens/main_screen.hpp"

#include "gui/dsl.hpp"

namespace Leticia::screens {

namespace {

using namespace Leticia::units;
using Leticia::ui::button;
using Leticia::ui::widget;

/**
 * @brief Wakes the display and resets the idle timer on any input activity.
 */
void user_activity_event_cb(lv_event_t *e) {
    auto *power = static_cast<Leticia::power_manager *>(lv_event_get_user_data(e));

    Leticia::power_state state = power->get_state();
    if (state == Leticia::power_state::sleep || state == Leticia::power_state::dimmed)
        power->set_state(Leticia::power_state::on);

    power->reset_activity_timer();
}

/**
 * @brief Toggles the ALSA test tone and updates the button's label to match.
 */
void audio_toggle_btn_event_cb(lv_event_t *e) {
    auto *audio = static_cast<Leticia::audio_manager *>(lv_event_get_user_data(e));
    auto *btn = static_cast<lv_obj_t *>(lv_event_get_target(e));
    lv_obj_t *btn_label = lv_obj_get_child(btn, 0);

    if (!audio->is_available()) {
        lv_label_set_text(btn_label, "Audio Unavailable");
        return;
    }

    bool playing = audio->toggle_test_tone();
    lv_label_set_text(btn_label, playing ? "Stop 1kHz Sine" : "Play 1kHz Sine");
}

/**
 * @brief Requests the main loop to exit, watched from the LVGL thread.
 */
void exit_btn_event_cb(lv_event_t *e) {
    auto *flag = static_cast<std::atomic<bool> *>(lv_event_get_user_data(e));
    flag->store(true, std::memory_order_relaxed);
}

} // namespace

void build_main_screen(Leticia::audio_manager &audio, Leticia::power_manager &power, std::atomic<bool> &should_exit) {
    widget scr(lv_screen_active());

    button audio_btn(scr);
    audio_btn.text(audio.is_available() ? "Play 1kHz Sine" : "Audio Unavailable", 24_sp)
        .size(260_dp, 90_dp)
        .align(LV_ALIGN_CENTER, 0_dp, -60_dp)
        .on(LV_EVENT_CLICKED, audio_toggle_btn_event_cb, &audio)
        .on(LV_EVENT_ALL, user_activity_event_cb, &power);

    button exit_btn(scr);
    exit_btn.text("Exit", 24_sp)
        .size(220_dp, 90_dp)
        .align(LV_ALIGN_CENTER, 0_dp, 80_dp)
        .on(LV_EVENT_CLICKED, exit_btn_event_cb, &should_exit)
        .on(LV_EVENT_ALL, user_activity_event_cb, &power);

    scr.on(LV_EVENT_ALL, user_activity_event_cb, &power);
}

} // namespace Leticia::screens
