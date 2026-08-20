#include "power_mgr.hpp"

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <unistd.h>

namespace Leticia {

namespace {

constexpr uint32_t kDefaultSleepTimeoutSec = 30;
constexpr uint32_t kDefaultDimTimeoutSec = 20;
constexpr uint32_t kDimHoldMs = 5000;
constexpr int kBrightnessFadeMs = 150;
constexpr int kBrightnessFadeStepMs = 15;

uint64_t now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + (ts.tv_nsec / 1000000);
}

} // namespace

power_manager::~power_manager()
{
    deinit();
}

bool power_manager::set_display_blank_sysfs(bool blank)
{
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
            fprintf(stderr, "sysfs blanking %s via %s\n", blank ? "OFF" : "ON", path);
            return true;
        }
    }
    return false;
}

bool power_manager::set_display_blank(bool blank)
{
    if (fb_fd_ >= 0) {
        int blank_mode = blank ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK;
        if (ioctl(fb_fd_, FBIOBLANK, blank_mode) == 0) {
            fprintf(stderr, "ioctl blanking %s\n", blank ? "OFF" : "ON");
            return true;
        }
        perror("ioctl(FBIOBLANK)");
    }

    if (set_display_blank_sysfs(blank))
        return true;

    fprintf(stderr, "All blanking methods failed\n");
    return false;
}

bool power_manager::find_backlight()
{
    DIR *dir = opendir("/sys/class/backlight");
    if (dir == nullptr)
        return false;

    bool found = false;
    struct dirent *entry;

    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.')
            continue;

        char max_path[160];
        char brightness_path[160];
        snprintf(max_path, sizeof(max_path), "/sys/class/backlight/%s/max_brightness", entry->d_name);
        snprintf(brightness_path, sizeof(brightness_path), "/sys/class/backlight/%s/brightness", entry->d_name);

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
        found = true;
        fprintf(stderr, "Using backlight: %s (max %d)\n", brightness_path, max_val);
        break;
    }

    closedir(dir);
    return found;
}

int power_manager::read_current_brightness_percent()
{
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

void power_manager::write_brightness_percent(int percent)
{
    if (backlight_path_.empty())
        return;

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    int raw = (percent * max_brightness_) / 100;

    int fd = open(backlight_path_.c_str(), O_WRONLY);
    if (fd < 0)
        return;

    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d", raw);
    write(fd, buf, static_cast<size_t>(len));
    close(fd);

    brightness_percent_ = percent;
}

void power_manager::fade_timer_trampoline(lv_timer_t *timer)
{
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

void power_manager::fade_brightness_to(int target_percent, std::function<void()> on_complete)
{
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

void power_manager::cancel_dim_hold()
{
    if (dim_hold_timer_ != nullptr) {
        lv_timer_delete(dim_hold_timer_);
        dim_hold_timer_ = nullptr;
    }
}

void power_manager::dim_hold_timer_trampoline(lv_timer_t *timer)
{
    auto *self = static_cast<power_manager *>(lv_timer_get_user_data(timer));
    self->dim_hold_timer_ = nullptr;

    if (self->current_state_ == power_state::dimmed)
        self->set_state(power_state::sleep);
}

void power_manager::finish_sleep_after_fade()
{
    if (set_display_blank(true)) {
        current_state_ = power_state::sleep;
        fprintf(stderr, "Display turned OFF (sleep)\n");
    } else {
        fprintf(stderr, "Failed to blank display after fade\n");
    }
}

void power_manager::on_input_event(const input_event_t &event)
{
    if (!pwr_button_enabled_)
        return;

    /* Volume, headphone and USB events are decoded by input_monitor_ too,
     * ready for future consumers; only the power button drives sleep/wake
     * today. */
    if (event.type == input_event_type::power_button) {
        toggle_sleep();
        reset_activity_timer();
    }
}

void power_manager::activity_timer_trampoline(lv_timer_t *timer)
{
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
            fprintf(stderr, "Auto-dim triggered after %lu seconds of inactivity\n",
                    static_cast<unsigned long>(self->dim_timeout_sec_));
            self->set_state(power_state::dimmed);
        } else if (!dim_enabled && self->sleep_timeout_sec_ > 0 && idle_sec >= self->sleep_timeout_sec_) {
            fprintf(stderr, "Auto-sleep triggered after %lu seconds of inactivity\n",
                    static_cast<unsigned long>(self->sleep_timeout_sec_));
            self->set_state(power_state::sleep);
        }
    }
    /* Once dimmed, the dim_hold_timer owns escalation to sleep on its own
     * fixed delay, independent of sleep_timeout_sec_. */
}

void power_manager::init(lv_display_t *disp)
{
    if (initialized_)
        return;

    disp_ = disp;
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
        fprintf(stderr, "No controllable backlight found, dim will fall back to blanking\n");
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
        fprintf(stderr, "Warning: Could not open framebuffer %s for power control\n", fb_path_.c_str());
    } else {
        fprintf(stderr, "Opened framebuffer %s for power control\n", fb_path_.c_str());
    }

    if (input_monitor_.open()) {
        input_monitor_.set_callback([this](const input_event_t &event) { on_input_event(event); });
    } else {
        fprintf(stderr, "No power button found, button control disabled\n");
    }

    activity_timer_ = lv_timer_create(activity_timer_trampoline, 500, this);

    initialized_ = true;

    fprintf(stderr, "Power management initialized\n");
}

bool power_manager::set_state(power_state state)
{
    if (!initialized_)
        return false;

    if (state == current_state_)
        return true;

    switch (state) {
        case power_state::on: {
            if (!set_display_blank(false)) {
                fprintf(stderr, "Failed to unblank display, state unchanged\n");
                return false;
            }
            current_state_ = power_state::on;
            fprintf(stderr, "Display turned ON\n");
            cancel_dim_hold();
            if (fade_timer_ != nullptr) {
                lv_timer_delete(fade_timer_);
                fade_timer_ = nullptr;
            }
            fade_complete_cb_ = nullptr;
            /* Wake is instant, no fade-in, matches real hardware. */
            write_brightness_percent(100);
            lv_obj_invalidate(lv_screen_active());
            lv_timer_handler();
            break;
        }

        case power_state::dimmed: {
            /* Panel stays unblanked, only the backlight fades down to half
             * of whatever it currently is, then holds there for
             * kDimHoldMs before escalating to sleep on its own. */
            current_state_ = power_state::dimmed;
            fprintf(stderr, "Display DIMMED\n");
            int current = read_current_brightness_percent();
            fade_brightness_to(current / 2);
            cancel_dim_hold();
            dim_hold_timer_ = lv_timer_create(dim_hold_timer_trampoline, kDimHoldMs, this);
            lv_timer_set_repeat_count(dim_hold_timer_, 1);
            break;
        }

        case power_state::sleep:
            cancel_dim_hold();
            /* Fade whatever backlight is left down to black, then blank the
             * panel only once that fade completes, instead of cutting
             * straight to off. */
            fade_brightness_to(0, [this] { finish_sleep_after_fade(); });
            return true;

        case power_state::deep_sleep:
            fprintf(stderr, "Deep sleep not implemented\n");
            return false;
    }

    /* Only reset the idle clock when waking to fully on. Resetting it on
     * dimmed or sleep would restart the countdown and the screen would
     * never progress from dimmed to off on its own. */
    if (state == power_state::on)
        reset_activity_timer();

    return true;
}

power_state power_manager::get_state() const
{
    return current_state_;
}

power_state power_manager::toggle_sleep()
{
    if (!initialized_)
        return current_state_;

    bool currently_awake = current_state_ == power_state::on || current_state_ == power_state::dimmed;
    power_state new_state = currently_awake ? power_state::sleep : power_state::on;

    set_state(new_state);
    reset_activity_timer();

    return new_state;
}

void power_manager::set_dim_timeout(uint32_t timeout_seconds)
{
    dim_timeout_sec_ = timeout_seconds;
    fprintf(stderr, "Dim timeout set to %u seconds\n", timeout_seconds);
}

void power_manager::set_sleep_timeout(uint32_t timeout_seconds)
{
    sleep_timeout_sec_ = timeout_seconds;
    fprintf(stderr, "Sleep timeout set to %u seconds\n", timeout_seconds);
}

void power_manager::reset_activity_timer()
{
    last_activity_time_ms_ = now_ms();
}

void power_manager::set_pwr_button_enabled(bool enabled)
{
    pwr_button_enabled_ = enabled;
    fprintf(stderr, "Power button %s\n", enabled ? "enabled" : "disabled");
}

void power_manager::deinit()
{
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
    fprintf(stderr, "Power management deinitialized\n");
}

} // namespace Leticia
