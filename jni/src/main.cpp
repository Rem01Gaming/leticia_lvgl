#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>

#include <lvgl.h>

#include "audio/audio_manager.hpp"
#include "config/device_config.hpp"
#include "gui/screens/main_screen.hpp"
#include "gui/ui_scale.hpp"
#include "gui/units.hpp"
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

#if LV_USE_LOG
/**
 * @brief Forwards LVGL's internal log messages to the recovery UI pipe.
 *
 * The default log target is stderr, which is not captured by recovery.log
 * in this environment, so LVGL's own diagnostics (fbdev open/mmap status,
 * resolution, color depth, draw dispatch warnings) would otherwise be
 * invisible. This is temporary/debug instrumentation for the 9.6 port.
 */
void lvgl_log_cb(lv_log_level_t level, const char *buf) {
    (void)level;
    Leticia::ui_print("lvgl: %s", buf);
}
#endif

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
        if (lv_linux_fbdev_set_file(disp, env_override) != LV_RESULT_OK) {
            Leticia::ui_print("error: failed to map framebuffer device %s", env_override);
            lv_display_delete(disp);
            return nullptr;
        }
        lv_linux_fbdev_set_force_refresh(disp, true);
        return disp;
    }

    for (const char *candidate : candidates) {
        if (access(candidate, F_OK) != 0)
            continue;

        if (lv_linux_fbdev_set_file(disp, candidate) != LV_RESULT_OK) {
            Leticia::ui_print("error: failed to map framebuffer device %s", candidate);
            continue;
        }
        lv_linux_fbdev_set_force_refresh(disp, true);
        return disp;
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
 * @brief Sets the display DPI and derives the dp/sp density used by every screen.
 *
 * @param disp LVGL display pointer.
 */
void apply_display_density(lv_display_t *disp) {
    int32_t hor_res = lv_display_get_horizontal_resolution(disp);
    int32_t ver_res = lv_display_get_vertical_resolution(disp);

    int dpi = Leticia::ui_scale::estimate_dpi(hor_res, ver_res);
    lv_display_set_dpi(disp, dpi);
    Leticia::units::init(dpi);
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

#if LV_USE_LOG
    lv_log_register_print_cb(lvgl_log_cb);
#endif

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

    apply_display_density(disp);

    Leticia::power_manager power;
    Leticia::screens::build_main_screen(audio, power, g_should_exit);

    power.init(disp, device_config);
    power.set_touch_indev(indev);

    Leticia::safety_exit_monitor safety_exit;
    safety_exit.start(safety_exit_cb);

    Leticia::ui_print("Entering main loop (draw_unit_cnt=%d)", LV_DRAW_SW_DRAW_UNIT_CNT);

    bool first_frame_logged = false;
    while (!g_should_exit.load(std::memory_order_relaxed)) {
        uint32_t idle_ms = lv_timer_handler();

        if (!first_frame_logged) {
            Leticia::ui_print("First lv_timer_handler() call returned (idle_ms=%u)", idle_ms);
            first_frame_logged = true;
        }

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
