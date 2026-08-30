#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>

#include <lvgl.h>

#include "audio/audio_manager.hpp"
#include "config/device_config.hpp"
#include "gui/ui_scale.hpp"
#include "input/safety_exit.hpp"
#include "input/touch_probe.hpp"
#include "power/power_manager.hpp"
#include "util/parent_mute.hpp"
#include "util/updater_proto.hpp"

namespace {

std::atomic<bool> g_should_exit{false};

void request_exit(int signum) {
    (void)signum;
    g_should_exit.store(true, std::memory_order_relaxed);
}

void exit_btn_event_cb(lv_event_t *e) {
    (void)e;
    g_should_exit.store(true, std::memory_order_relaxed);
}

/**
 * @brief Requests an exit from the safety exit watchdog thread.
 *
 * Runs on the watchdog thread, not the LVGL main thread, so it must stay lock-free.
 */
void safety_exit_cb() {
    Leticia::ui_print("Safety exit long-press triggered, closing UI");
    g_should_exit.store(true, std::memory_order_relaxed);
}

/**
 * @brief Handles user activity events to reset timers and wake the display.
 *
 * @param e LVGL event pointer.
 */
void user_activity_event_cb(lv_event_t *e) {
    auto *power = static_cast<Leticia::power_manager *>(lv_event_get_user_data(e));

    Leticia::power_state state = power->get_state();
    if (state == Leticia::power_state::sleep || state == Leticia::power_state::dimmed)
        power->set_state(Leticia::power_state::on);

    power->reset_activity_timer();
}

/**
 * @brief Toggles the test tone and updates the UI button.
 *
 * @param e LVGL event pointer.
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
 * @brief Opens the framebuffer display device.
 *
 * @return Pointer to the LVGL display or nullptr on failure.
 */
lv_display_t *open_fbdev_display() {
    static const char *candidates[] = {
        "/dev/graphics/fb0",
        "/dev/fb0",
    };

    const char *env_override = getenv("FB_DEVICE");
    lv_display_t *disp = lv_linux_fbdev_create();
    if (disp == nullptr)
        return nullptr;

    if (env_override != nullptr) {
        lv_linux_fbdev_set_file(disp, env_override);
        lv_linux_fbdev_set_force_refresh(disp, true);
        return disp;
    }

    for (const char *candidate : candidates) {
        if (access(candidate, F_OK) == 0) {
            lv_linux_fbdev_set_file(disp, candidate);
            lv_linux_fbdev_set_force_refresh(disp, true);
            return disp;
        }
    }

    lv_display_delete(disp);
    return nullptr;
}

/**
 * @brief Attaches the touchscreen input device.
 *
 * @return Pointer to the LVGL input device or nullptr if not found.
 */
lv_indev_t *open_touch_indev() {
    const char *env_override = getenv("TOUCH_DEVICE");
    if (env_override != nullptr)
        return lv_evdev_create(LV_INDEV_TYPE_POINTER, env_override);

    auto path = Leticia::touch_probe::find();
    if (!path) {
        Leticia::ui_print("No touchscreen node found, UI will be display only");
        return nullptr;
    }

    Leticia::ui_print("Using touch input: %s", path->c_str());
    return lv_evdev_create(LV_INDEV_TYPE_POINTER, path->c_str());
}

/**
 * @brief Pins the calling thread to the CPUs specified in config.
 * @param cpus Vector of exactly 2 CPU IDs.
 */
void apply_lvgl_cpu_affinity(const std::vector<int> &cpus) {
    if (cpus.empty()) {
        Leticia::ui_print("apply_audio_cpu_affinity: no LVGL thread affinity specified, skipping thread pin");
        return;
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    for (int cpu_id : cpus) {
        if (cpu_id < 0 || cpu_id >= CPU_SETSIZE) {
            Leticia::ui_print("apply_audio_cpu_affinity: CPU ID %d out of range [0-%d]", cpu_id, CPU_SETSIZE - 1);
            return;
        }
        CPU_SET(cpu_id, &cpuset);
    }

    if (sched_setaffinity(syscall(SYS_gettid), sizeof(cpuset), &cpuset) != 0) {
        Leticia::ui_print("apply_audio_cpu_affinity: sched_setaffinity: %s", strerror(errno));
        return;
    }

    std::string cpu_list;
    for (size_t i = 0; i < cpus.size(); i++) {
        if (i > 0) cpu_list += ", ";
        cpu_list += std::to_string(cpus[i]);
    }
    Leticia::ui_print("apply_lvgl_cpu_affinity: LVGL thread pinned to CPU(s): %s", cpu_list.c_str());
}

/**
 * @brief Builds the user interface.
 *
 * @param disp LVGL display pointer.
 * @param power Power manager instance.
 * @param audio Audio manager instance.
 */
void build_ui(lv_display_t *disp, Leticia::power_manager &power, Leticia::audio_manager &audio) {
    int32_t hor_res = lv_display_get_horizontal_resolution(disp);
    int32_t ver_res = lv_display_get_vertical_resolution(disp);

    int dpi = Leticia::ui_scale::estimate_dpi(hor_res, ver_res);
    lv_display_set_dpi(disp, dpi);

    float scale = Leticia::ui_scale::factor(dpi);

    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *audio_btn = lv_button_create(scr);
    lv_obj_set_size(audio_btn, static_cast<int>(260 * scale), static_cast<int>(90 * scale));
    lv_obj_align(audio_btn, LV_ALIGN_CENTER, 0, static_cast<int>(-60 * scale));
    lv_obj_add_event_cb(audio_btn, audio_toggle_btn_event_cb, LV_EVENT_CLICKED, &audio);
    lv_obj_add_event_cb(audio_btn, user_activity_event_cb, LV_EVENT_ALL, &power);

    lv_obj_t *audio_btn_label = lv_label_create(audio_btn);
    lv_label_set_text(audio_btn_label, audio.is_available() ? "Play 1kHz Sine" : "Audio Unavailable");
    lv_obj_set_style_text_font(audio_btn_label, Leticia::ui_scale::pick_font(static_cast<int>(24 * scale)), 0);
    lv_obj_center(audio_btn_label);

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, static_cast<int>(220 * scale), static_cast<int>(90 * scale));
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, static_cast<int>(80 * scale));
    lv_obj_add_event_cb(btn, exit_btn_event_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(btn, user_activity_event_cb, LV_EVENT_ALL, &power);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Exit");
    lv_obj_set_style_text_font(btn_label, Leticia::ui_scale::pick_font(static_cast<int>(24 * scale)), 0);
    lv_obj_center(btn_label);

    lv_obj_add_event_cb(scr, user_activity_event_cb, LV_EVENT_ALL, &power);
}

} // namespace

int main(int argc, char *argv[]) {
    Leticia::updater_proto proto;

    std::string zip_path;
    if (argc >= 4) {
        int pipe_fd = atoi(argv[2]);
        proto.attach(pipe_fd);
        Leticia::set_updater_proto(&proto);
        Leticia::ui_print("Launching Leticia UI");
        zip_path = argv[3];
    }

    signal(SIGINT, request_exit);
    signal(SIGTERM, request_exit);

    Leticia::parent_mute mute;
    mute.freeze();

    lv_init();

    lv_display_t *disp = open_fbdev_display();
    if (disp == nullptr) {
        Leticia::ui_print("error: could not open a framebuffer device");
        return 1;
    }

    lv_indev_t *indev = open_touch_indev();

    Leticia::device_config_t device_config;
    if (Leticia::load_device_config(zip_path, device_config)) {
        Leticia::ui_print("Loaded device config (backlight=%s)", device_config.backlight_path.c_str());
    } else {
        Leticia::ui_print("No device config found, using auto-detection");
    }

    Leticia::audio_manager audio;
    if (audio.init(device_config, zip_path)) {
        Leticia::ui_print("Audio ready (output: %s)",
                       audio.detected_output() == Leticia::audio_output::headphones ? "Headphones" : "Speaker");
    } else {
        Leticia::ui_print("No UCM config found, audio test unavailable");
    }

    apply_lvgl_cpu_affinity(device_config.lvgl_thread_affinity);

    Leticia::power_manager power;
    build_ui(disp, power, audio);

    power.init(disp, device_config);
    power.set_touch_indev(indev);

    Leticia::safety_exit_monitor safety_exit;
    safety_exit.start(safety_exit_cb);

    while (!g_should_exit.load(std::memory_order_relaxed)) {
        uint32_t idle_ms = lv_timer_handler();

        power.service_pending_wake();

        if (idle_ms == LV_NO_TIMER_READY)
            idle_ms = 50;
        usleep(idle_ms * 1000);
    }

    safety_exit.stop();

    power.deinit();
    audio.deinit();

    if (indev != nullptr)
        lv_evdev_delete(indev);

    lv_display_delete(disp);

    mute.resume();

    Leticia::ui_print("Leticia UI closed, returning to recovery");
    Leticia::clear_updater_proto();
    proto.close();

    return 0;
}
