#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include <lvgl.h>

#include "device_config.hpp"
#include "parent_mute.hpp"
#include "power_mgr.hpp"
#include "touch_probe.hpp"
#include "ui_scale.hpp"
#include "updater_proto.hpp"

namespace {

volatile sig_atomic_t g_should_exit = 0;

void request_exit(int signum)
{
    (void)signum;
    g_should_exit = 1;
}

void exit_btn_event_cb(lv_event_t *e)
{
    (void)e;
    g_should_exit = 1;
}

/**
 * @brief Callback for user interactions to reset activity timer and wake display if sleeping
 */
void user_activity_event_cb(lv_event_t *e)
{
    auto *power = static_cast<Leticia::power_manager *>(lv_event_get_user_data(e));

    Leticia::power_state state = power->get_state();
    if (state == Leticia::power_state::sleep || state == Leticia::power_state::dimmed)
        power->set_state(Leticia::power_state::on);

    power->reset_activity_timer();
}

/**
 * @brief Open the framebuffer device.
 * @return Newly created LVGL display, or nullptr on failure.
 */
lv_display_t *open_fbdev_display()
{
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
 * @brief Attach the touchscreen input device.
 * @return Newly created LVGL input device, or nullptr if none was found.
 */
lv_indev_t *open_touch_indev(Leticia::updater_proto &proto)
{
    const char *env_override = getenv("TOUCH_DEVICE");
    if (env_override != nullptr)
        return lv_evdev_create(LV_INDEV_TYPE_POINTER, env_override);

    auto path = Leticia::TouchProbe::find();
    if (!path) {
        proto.ui_print("No touchscreen node found, UI will be display only");
        return nullptr;
    }

    proto.ui_print("Using touch input: %s", path->c_str());
    return lv_evdev_create(LV_INDEV_TYPE_POINTER, path->c_str());
}

void build_ui(lv_display_t *disp, Leticia::power_manager &power)
{
    int32_t hor_res = lv_display_get_horizontal_resolution(disp);
    int32_t ver_res = lv_display_get_vertical_resolution(disp);

    int dpi = Leticia::UiScale::estimate_dpi(hor_res, ver_res);
    lv_display_set_dpi(disp, dpi);

    float scale = Leticia::UiScale::factor(dpi);

    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "Hello, World!");
    lv_obj_set_style_text_font(label, Leticia::UiScale::pick_font(static_cast<int>(32 * scale)), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, static_cast<int>(-60 * scale));

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, static_cast<int>(220 * scale), static_cast<int>(90 * scale));
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, static_cast<int>(80 * scale));
    lv_obj_add_event_cb(btn, exit_btn_event_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(btn, user_activity_event_cb, LV_EVENT_ALL, &power);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Exit");
    lv_obj_set_style_text_font(btn_label, Leticia::UiScale::pick_font(static_cast<int>(24 * scale)), 0);
    lv_obj_center(btn_label);

    lv_obj_add_event_cb(scr, user_activity_event_cb, LV_EVENT_ALL, &power);
}

} // namespace

int main(int argc, char *argv[])
{
    Leticia::updater_proto proto;

    /* Recovery invokes us as `update-binary <api_version> <pipe_fd> <zip_path>`
     * (standard edify convention) since this binary IS update-binary, not a
     * script interpreter -- argv[3] is the path to the OTA zip on disk,
     * which is also where a per-device config can travel (see
     * device_config.hpp) so the same prebuilt binary works across devices
     * without recompilation. */
    std::string zip_path;
    if (argc >= 4) {
        int pipe_fd = atoi(argv[2]);
        proto.attach(pipe_fd);
        proto.ui_print("Launching Leticia UI");
        zip_path = argv[3];
    }

    signal(SIGINT, request_exit);
    signal(SIGTERM, request_exit);

    Leticia::parent_mute mute;
    mute.freeze();

    lv_init();

    lv_display_t *disp = open_fbdev_display();
    if (disp == nullptr) {
        fprintf(stderr, "error: could not open a framebuffer device\n");
        proto.ui_print("error: could not open a framebuffer device");
        return 1;
    }

    lv_indev_t *indev = open_touch_indev(proto);

    Leticia::device_config_t device_config;
    if (Leticia::load_device_config(zip_path, device_config)) {
        proto.ui_print("Loaded device config (backlight=%s)", device_config.backlight_path.c_str());
    } else {
        proto.ui_print("No device config found, using auto-detection");
    }

    Leticia::power_manager power;
    build_ui(disp, power);

    power.init(disp, device_config);
    power.set_touch_indev(indev);

    while (!g_should_exit) {
        uint32_t idle_ms = lv_timer_handler();
        if (idle_ms == LV_NO_TIMER_READY)
            idle_ms = 50;
        usleep(idle_ms * 1000);
    }

    power.deinit();

    if (indev != nullptr)
        lv_evdev_delete(indev);

    lv_display_delete(disp);

    mute.resume();

    proto.ui_print("Leticia UI closed, returning to recovery");
    proto.close();

    return 0;
}
