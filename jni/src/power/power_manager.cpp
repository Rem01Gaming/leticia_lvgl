#include "power/power_manager.hpp"
#include "util/updater_proto.hpp"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Leticia {

namespace {

constexpr uint32_t kDefaultSleepTimeoutSec = 30;
constexpr uint32_t kDefaultDimTimeoutSec = 20;
constexpr uint32_t kDimHoldMs = 5000;
constexpr int kBrightnessFadeMs = 120;
constexpr int kBrightnessFadeStepMs = 15;

uint64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + (ts.tv_nsec / 1000000);
}

bool led_name_looks_like_backlight(const char *name) {
    static const char *hints[] = {"lcd-backlight", "lcd_backlight", "wled", "backlight"};
    for (const char *hint : hints) {
        if (strstr(name, hint) != nullptr)
            return true;
    }
    return false;
}

} // namespace

power_manager::~power_manager() {
    deinit();
}

bool power_manager::set_display_blank_sysfs(bool blank) {
    static const char *sysfs_paths[] = {
        "/sys/class/graphics/fb0/blank",
        "/sys/class/graphics/fb1/blank",
    };

    for (const char *path : sysfs_paths) {
        int fd = open(path, O_WRONLY);
        if (fd < 0)
            continue;

        const char *val = blank ? "1" : "0";
        ssize_t ret = write(fd, val, strlen(val));
        close(fd);
        if (ret > 0) {
            Leticia::ui_print("sysfs blanking %s via %s", blank ? "OFF" : "ON", path);
            return true;
        }
    }
    return false;
}

bool power_manager::set_display_blank(bool blank) {
    if (!device_config_.screen_blank_supported) {
        return true;
    }

    if (fb_fd_ >= 0) {
        int blank_mode = blank ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK;
        if (ioctl(fb_fd_, FBIOBLANK, blank_mode) == 0) {
            Leticia::ui_print("ioctl blanking %s", blank ? "OFF" : "ON");
            return true;
        }
        perror("ioctl(FBIOBLANK)");
    }

    if (set_display_blank_sysfs(blank))
        return true;

    Leticia::ui_print("All blanking methods failed");
    return false;
}

/**
 * @brief Clears the framebuffer pixel memory to black.
 */
void power_manager::blank_framebuffer_pixels() {
    if (fb_fd_ < 0)
        return;

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    if (ioctl(fb_fd_, FBIOGET_VSCREENINFO, &vinfo) != 0) {
        perror("blank_framebuffer_pixels: FBIOGET_VSCREENINFO");
        return;
    }
    if (ioctl(fb_fd_, FBIOGET_FSCREENINFO, &finfo) != 0) {
        perror("blank_framebuffer_pixels: FBIOGET_FSCREENINFO");
        return;
    }

    size_t screensize = static_cast<size_t>(finfo.smem_len);
    if (screensize == 0) {
        screensize = static_cast<size_t>(vinfo.yres_virtual) * finfo.line_length;
    }
    if (screensize == 0)
        return;

    void *mem = mmap(nullptr, screensize, PROT_WRITE, MAP_SHARED, fb_fd_, 0);
    if (mem == MAP_FAILED) {
        perror("blank_framebuffer_pixels: mmap");
        return;
    }

    memset(mem, 0, screensize);

    munmap(mem, screensize);
    Leticia::ui_print("Framebuffer pixels cleared to black (%zu bytes)", screensize);
}

bool power_manager::find_backlight() {
    if (device_config_.configured()) {
        int fd = open(device_config_.backlight_path.c_str(), O_WRONLY);
        if (fd < 0) {
            perror("device_config backlight_path: open");
        } else {
            close(fd);
            backlight_path_ = device_config_.backlight_path;
            max_brightness_ = device_config_.max_brightness > 0 ? device_config_.max_brightness : 255;
            Leticia::ui_print("Using configured backlight: %s (max %d)", backlight_path_.c_str(), max_brightness_);
            return true;
        }
        Leticia::ui_print("Configured backlight_path unusable, falling back to auto-detect");
    }

    /**
     * @brief Probes for available backlight devices.
     */
    static const char *class_dirs[] = {
        "/sys/class/backlight",
        "/sys/class/leds",
    };

    for (const char *class_dir : class_dirs) {
        DIR *dir = opendir(class_dir);
        if (dir == nullptr)
            continue;

        bool is_led_dir = strcmp(class_dir, "/sys/class/leds") == 0;
        struct dirent *entry;

        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.')
                continue;

            if (is_led_dir && !led_name_looks_like_backlight(entry->d_name))
                continue;

            char max_path[PATH_MAX];
            char brightness_path[PATH_MAX];
            snprintf(max_path, sizeof(max_path), "%s/%s/max_brightness", class_dir, entry->d_name);
            snprintf(brightness_path, sizeof(brightness_path), "%s/%s/brightness", class_dir, entry->d_name);

            FILE *f = fopen(max_path, "r");
            if (f == nullptr)
                continue;

            int max_val = 0;
            int scanned = fscanf(f, "%d", &max_val);
            fclose(f);

            if (scanned != 1 || max_val <= 0)
                continue;

            int fd = open(brightness_path, O_WRONLY);
            if (fd < 0)
                continue;
            close(fd);

            backlight_path_ = brightness_path;
            max_brightness_ = max_val;
            Leticia::ui_print("Using auto-detected backlight: %s (max %d)", brightness_path, max_val);
            closedir(dir);
            return true;
        }

        closedir(dir);
    }

    return false;
}

int power_manager::read_current_brightness_percent() {
    if (backlight_path_.empty())
        return brightness_percent_;

    FILE *f = fopen(backlight_path_.c_str(), "r");
    if (f == nullptr)
        return brightness_percent_;

    int raw = 0;
    int scanned = fscanf(f, "%d", &raw);
    fclose(f);

    if (scanned != 1 || max_brightness_ <= 0)
        return brightness_percent_;

    int percent = (raw * 100) / max_brightness_;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    brightness_percent_ = percent;
    return percent;
}

void power_manager::write_brightness_percent(int percent) {
    if (backlight_path_.empty())
        return;

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    int raw = (percent * max_brightness_) / 100;

    int fd = open(backlight_path_.c_str(), O_WRONLY);
    if (fd < 0) {
        perror("write_brightness_percent: open");
        return;
    }

    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d", raw);
    ssize_t written = write(fd, buf, static_cast<size_t>(len));
    close(fd);

    if (written != len) {
        perror("write_brightness_percent: write");
        return;
    }

    brightness_percent_ = percent;
}

void power_manager::fade_timer_trampoline(lv_timer_t *timer) {
    auto *self = static_cast<power_manager *>(lv_timer_get_user_data(timer));

    uint64_t elapsed = now_ms() - self->fade_start_ms_;

    if (elapsed >= static_cast<uint64_t>(kBrightnessFadeMs)) {
        self->write_brightness_percent(self->fade_to_percent_);
        lv_timer_delete(timer);
        self->fade_timer_ = nullptr;

        auto cb = std::move(self->fade_complete_cb_);
        self->fade_complete_cb_ = nullptr;
        if (cb)
            cb();
        return;
    }

    int delta = self->fade_to_percent_ - self->fade_from_percent_;
    int percent = self->fade_from_percent_ + static_cast<int>((delta * static_cast<int64_t>(elapsed)) / kBrightnessFadeMs);
    self->write_brightness_percent(percent);
}

void power_manager::fade_brightness_to(int target_percent, std::function<void()> on_complete) {
    if (backlight_path_.empty()) {
        if (on_complete)
            on_complete();
        return;
    }

    if (fade_timer_ != nullptr) {
        lv_timer_delete(fade_timer_);
        fade_timer_ = nullptr;
    }

    fade_from_percent_ = read_current_brightness_percent();
    fade_to_percent_ = target_percent;
    fade_start_ms_ = now_ms();
    fade_complete_cb_ = std::move(on_complete);

    if (fade_from_percent_ == fade_to_percent_) {
        auto cb = std::move(fade_complete_cb_);
        fade_complete_cb_ = nullptr;
        if (cb)
            cb();
        return;
    }

    fade_timer_ = lv_timer_create(fade_timer_trampoline, kBrightnessFadeStepMs, this);
}

void power_manager::cancel_dim_hold() {
    if (dim_hold_timer_ != nullptr) {
        lv_timer_delete(dim_hold_timer_);
        dim_hold_timer_ = nullptr;
    }
}

void power_manager::dim_hold_timer_trampoline(lv_timer_t *timer) {
    auto *self = static_cast<power_manager *>(lv_timer_get_user_data(timer));
    self->dim_hold_timer_ = nullptr;

    if (self->current_state_ == power_state::dimmed)
        self->set_state(power_state::sleep);
}

void power_manager::finish_sleep_after_fade() {
    if (!set_display_blank(true))
        Leticia::ui_print("Warning: fb blank request failed, continuing in backlight-off sleep");

    blank_framebuffer_pixels();

    current_state_ = power_state::sleep;
    Leticia::ui_print("Display turned OFF (sleep)");
}

void power_manager::set_touch_indev(lv_indev_t *indev) {
    touch_indev_ = indev;

    set_touch_enabled(current_state_ != power_state::sleep);
}

void power_manager::set_touch_enabled(bool enabled) {
    if (touch_indev_ == nullptr)
        return;

    lv_indev_enable(touch_indev_, enabled);
}

void power_manager::on_input_event(const input_event_t &event) {
    if (!pwr_button_enabled_)
        return;

    if (event.type == input_event_type::power_button) {
        toggle_sleep();
        reset_activity_timer();
    }
}

void power_manager::activity_timer_trampoline(lv_timer_t *timer) {
    auto *self = static_cast<power_manager *>(lv_timer_get_user_data(timer));

    if (!self->initialized_)
        return;

    self->input_monitor_.poll();

    if (self->sleep_timeout_sec_ == 0 && self->dim_timeout_sec_ == 0)
        return;

    uint64_t now = now_ms();
    uint64_t idle_sec = (now - self->last_activity_time_ms_) / 1000;

    if (self->current_state_ == power_state::on) {
        bool dim_enabled = self->dim_timeout_sec_ > 0 && self->dim_timeout_sec_ < self->sleep_timeout_sec_;

        if (dim_enabled && idle_sec >= self->dim_timeout_sec_) {
            Leticia::ui_print("Auto-dim triggered after %lu seconds of inactivity", static_cast<unsigned long>(self->dim_timeout_sec_));
            self->set_state(power_state::dimmed);
        } else if (!dim_enabled && self->sleep_timeout_sec_ > 0 && idle_sec >= self->sleep_timeout_sec_) {
            Leticia::ui_print("Auto-sleep triggered after %lu seconds of inactivity", static_cast<unsigned long>(self->sleep_timeout_sec_));
            self->set_state(power_state::sleep);
        }
    }
}

void power_manager::init(lv_display_t *disp, const device_config_t &config) {
    if (initialized_)
        return;

    disp_ = disp;
    device_config_ = config;
    current_state_ = power_state::on;
    sleep_timeout_sec_ = kDefaultSleepTimeoutSec;
    dim_timeout_sec_ = kDefaultDimTimeoutSec;
    pwr_button_enabled_ = true;
    last_activity_time_ms_ = now_ms();
    brightness_percent_ = 100;
    max_brightness_ = 255;

    if (find_backlight()) {
        write_brightness_percent(100);
    } else {
        Leticia::ui_print("No controllable backlight found, sleep/dim will have no visible effect");
    }

    const char *fb_path = getenv("FB_DEVICE");
    if (fb_path == nullptr) {
        if (access("/dev/graphics/fb0", F_OK) == 0)
            fb_path = "/dev/graphics/fb0";
        else if (access("/dev/fb0", F_OK) == 0)
            fb_path = "/dev/fb0";
        else
            fb_path = "/dev/fb0";
    }
    fb_path_ = fb_path;

    fb_fd_ = open(fb_path_.c_str(), O_RDWR);
    if (fb_fd_ < 0) {
        Leticia::ui_print("Warning: Could not open framebuffer %s for power control", fb_path_.c_str());
    } else {
        Leticia::ui_print("Opened framebuffer %s for power control", fb_path_.c_str());
    }

    if (input_monitor_.open()) {
        input_monitor_.set_callback([this](const input_event_t &event) {
            on_input_event(event);
        });
    } else {
        Leticia::ui_print("No power button found, button control disabled");
    }

    activity_timer_ = lv_timer_create(activity_timer_trampoline, 500, this);

    initialized_ = true;

    Leticia::ui_print("Power management initialized (screen_blank_supported=%s)", device_config_.screen_blank_supported ? "true" : "false");
}

bool power_manager::set_state(power_state state) {
    if (!initialized_)
        return false;

    if (state == current_state_)
        return true;

    switch (state) {
        case power_state::on: {
            if (!set_display_blank(false))
                Leticia::ui_print("Warning: fb unblank request failed, backlight restored anyway");

            current_state_ = power_state::on;
            Leticia::ui_print("Display turned ON");
            set_touch_enabled(true);
            cancel_dim_hold();
            if (fade_timer_ != nullptr) {
                lv_timer_delete(fade_timer_);
                fade_timer_ = nullptr;
            }
            fade_complete_cb_ = nullptr;
            lv_obj_invalidate(lv_screen_active());
            /* Don't touch the backlight or call into LVGL's redraw/timer
             * machinery here -- set_state() can be reached re-entrantly
             * from inside an LVGL timer callback, where that's unsafe.
             * Just flag the wake as pending; service_pending_wake() does
             * the full redraw + backlight restore, in that order, from
             * the main loop. */
            pending_full_redraw_ = true;
            break;
        }

        case power_state::dimmed: {
            current_state_ = power_state::dimmed;
            Leticia::ui_print("Display DIMMED");
            int current = read_current_brightness_percent();
            fade_brightness_to(current / 4);
            cancel_dim_hold();
            dim_hold_timer_ = lv_timer_create(dim_hold_timer_trampoline, kDimHoldMs, this);
            lv_timer_set_repeat_count(dim_hold_timer_, 1);
            break;
        }

        case power_state::sleep:
            cancel_dim_hold();
            current_state_ = power_state::sleep;
            set_touch_enabled(false);
            fade_brightness_to(0, [this] {
                finish_sleep_after_fade();
            });
            return true;

        case power_state::deep_sleep:
            Leticia::ui_print("Deep sleep not implemented");
            return false;
    }

    // Only reset the idle clock on waking to fully "on" -- resetting it
    // on dimmed/sleep would prevent auto-progression to sleep.
    if (state == power_state::on)
        reset_activity_timer();

    return true;
}

power_state power_manager::get_state() const {
    return current_state_;
}

bool power_manager::service_pending_wake() {
    if (!pending_full_redraw_)
        return false;

    pending_full_redraw_ = false;

    if (disp_ != nullptr)
        lv_refr_now(disp_);

    write_brightness_percent(100);

    return true;
}

power_state power_manager::toggle_sleep() {
    if (!initialized_)
        return current_state_;

    bool currently_awake = current_state_ == power_state::on || current_state_ == power_state::dimmed;
    power_state new_state = currently_awake ? power_state::sleep : power_state::on;

    set_state(new_state);
    reset_activity_timer();

    return new_state;
}

void power_manager::set_dim_timeout(uint32_t timeout_seconds) {
    dim_timeout_sec_ = timeout_seconds;
    Leticia::ui_print("Dim timeout set to %u seconds", timeout_seconds);
}

void power_manager::set_sleep_timeout(uint32_t timeout_seconds) {
    sleep_timeout_sec_ = timeout_seconds;
    Leticia::ui_print("Sleep timeout set to %u seconds", timeout_seconds);
}

void power_manager::reset_activity_timer() {
    last_activity_time_ms_ = now_ms();
}

void power_manager::set_pwr_button_enabled(bool enabled) {
    pwr_button_enabled_ = enabled;
    Leticia::ui_print("Power button %s", enabled ? "enabled" : "disabled");
}

void power_manager::deinit() {
    if (!initialized_)
        return;

    if (activity_timer_ != nullptr) {
        lv_timer_delete(activity_timer_);
        activity_timer_ = nullptr;
    }

    if (fade_timer_ != nullptr) {
        lv_timer_delete(fade_timer_);
        fade_timer_ = nullptr;
    }
    fade_complete_cb_ = nullptr;

    cancel_dim_hold();

    input_monitor_.close();

    if (fb_fd_ >= 0) {
        close(fb_fd_);
        fb_fd_ = -1;
    }

    initialized_ = false;
    Leticia::ui_print("Power management deinitialized");
}

} // namespace Leticia
