#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lvgl.h>

#include "parent_mute.h"
#include "power_mgr.h"
#include "touch_probe.h"
#include "ui_scale.h"
#include "updater_proto.h"

static volatile sig_atomic_t g_should_exit = 0;

static void request_exit(int signum)
{
    (void)signum;
    g_should_exit = 1;
}

static void exit_btn_event_cb(lv_event_t *e)
{
    (void)e;
    g_should_exit = 1;
}

/**
 * @brief Callback for user interactions to reset activity timer and wake display if sleeping
 */
static void user_activity_event_cb(lv_event_t *e)
{
    (void)e;
    /* If display is sleeping or dimmed, wake it up */
    power_state_t state = power_mgmt_get_state();
    if (state == POWER_STATE_SLEEP || state == POWER_STATE_DIMMED) {
        power_mgmt_set_state(POWER_STATE_ON);
    }
    power_mgmt_reset_activity_timer();
}

/**
 * @brief Open the framebuffer device.
 * @return Newly created LVGL display, or NULL on failure.
 */
static lv_display_t *open_fbdev_display(void)
{
    static const char *candidates[] = {
        "/dev/graphics/fb0",
        "/dev/fb0",
    };

    const char *env_override = getenv("FB_DEVICE");
    lv_display_t *disp = lv_linux_fbdev_create();
    if (disp == NULL)
        return NULL;

    if (env_override != NULL) {
        lv_linux_fbdev_set_file(disp, env_override);
        lv_linux_fbdev_set_force_refresh(disp, true);
        return disp;
    }

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (access(candidates[i], F_OK) == 0) {
            lv_linux_fbdev_set_file(disp, candidates[i]);
            lv_linux_fbdev_set_force_refresh(disp, true);
            return disp;
        }
    }

    lv_display_delete(disp);
    return NULL;
}

/**
 * @brief Attach the touchscreen input device.
 * @return Newly created LVGL input device, or NULL if none was found.
 */
static lv_indev_t *open_touch_indev(void)
{
    char path[TOUCH_PROBE_MAX_PATH];

    const char *env_override = getenv("TOUCH_DEVICE");
    if (env_override != NULL)
        return lv_evdev_create(LV_INDEV_TYPE_POINTER, env_override);

    if (touch_probe_find(path, sizeof(path)) != 0) {
        updater_proto_ui_print("No touchscreen node found, UI will be display only");
        return NULL;
    }

    updater_proto_ui_print("Using touch input: %s", path);
    return lv_evdev_create(LV_INDEV_TYPE_POINTER, path);
}

static void build_ui(lv_display_t *disp)
{
    int32_t hor_res = lv_display_get_horizontal_resolution(disp);
    int32_t ver_res = lv_display_get_vertical_resolution(disp);

    int dpi = ui_scale_estimate_dpi(hor_res, ver_res);
    lv_display_set_dpi(disp, dpi);

    float scale = ui_scale_factor(dpi);

    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "Hello, World!");
    lv_obj_set_style_text_font(label, ui_scale_pick_font((int)(32 * scale)), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, (int)(-60 * scale));

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, (int)(220 * scale), (int)(90 * scale));
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, (int)(80 * scale));
    lv_obj_add_event_cb(btn, exit_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn, user_activity_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Exit");
    lv_obj_set_style_text_font(btn_label, ui_scale_pick_font((int)(24 * scale)), 0);
    lv_obj_center(btn_label);

    lv_obj_add_event_cb(scr, user_activity_event_cb, LV_EVENT_ALL, NULL);
}

int main(int argc, char *argv[])
{
    int pipe_fd = -1;

    if (argc >= 4) {
        pipe_fd = atoi(argv[2]);
        updater_proto_attach(pipe_fd);
        updater_proto_ui_print("Launching Leticia UI");
    }

    signal(SIGINT, request_exit);
    signal(SIGTERM, request_exit);

    parent_mute_freeze();

    lv_init();

    lv_display_t *disp = open_fbdev_display();
    if (disp == NULL) {
        fprintf(stderr, "error: could not open a framebuffer device\n");
        updater_proto_ui_print("error: could not open a framebuffer device");
        parent_mute_resume();
        updater_proto_close();
        return 1;
    }

    lv_indev_t *indev = open_touch_indev();

    build_ui(disp);

    /* Initialize power management */
    power_mgmt_init(disp);

    while (!g_should_exit) {
        uint32_t idle_ms = lv_timer_handler();
        if (idle_ms == LV_NO_TIMER_READY)
            idle_ms = 50;
        usleep(idle_ms * 1000);
    }

    /* Cleanup power management */
    power_mgmt_deinit();

    if (indev != NULL) {
        lv_evdev_delete(indev);
    }

    lv_display_delete(disp);

    parent_mute_resume();

    updater_proto_ui_print("Leticia UI closed, returning to recovery");
    updater_proto_close();

    return 0;
}
