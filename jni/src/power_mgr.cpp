#include "power_mgr.hpp"

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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

bool led_name_looks_like_backlight(const char *name)
{
    static const char *hints[] = {"lcd-backlight", "lcd_backlight", "wled", "backlight"};
    for (const char *hint : hints) {
        if (strstr(name, hint) != nullptr)
            return true;
    }
    return false;
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
    if (!device_config_.screen_blank_supported) {
        // Unconfigured devices, and any device explicitly marked
        // screen_blank_supported=false, skip fb-level blanking entirely.
        // Many panels (this codebase's original test device included, an
        // MTK mtkfb panel) either ignore FBIOBLANK/fb0-blank outright or
        // blank without ever reliably recovering on UNBLANK -- backlight
        // fade (see finish_sleep_after_fade()/set_state()) is the part of
        // "sleep" guaranteed to work everywhere, so that's the default.
        // Set screen_blank_supported=true in device.conf once a device has
        // actually been verified to blank AND recover correctly.
        return true;
    }

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

/**
 * @brief Paint the framebuffer's actual pixel memory black, independent of
 *        FBIOBLANK/backlight. On panels where screen_blank_supported is
 *        false (the common case -- see set_display_blank()), backlight
 *        fading to 0 stops the LED array from lighting the panel but does
 *        NOT clear whatever frame is still latched in the LCD itself, so
 *        the old UI stays faintly visible under a raking light. This is a
 *        one-shot direct write via mmap, bypassing LVGL's flush/timer
 *        pipeline entirely (safe to call from inside the fade-complete
 *        callback, which itself can run from inside an LVGL timer tick).
 */
void power_manager::blank_framebuffer_pixels()
{
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
        // Some drivers report a bogus/zero smem_len; fall back to a
        // computed estimate from the mode info rather than giving up.
        screensize = static_cast<size_t>(vinfo.yres_virtual) * finfo.line_length;
    }
    if (screensize == 0)
        return;

    void *mem = mmap(nullptr, screensize, PROT_WRITE, MAP_SHARED, fb_fd_, 0);
    if (mem == MAP_FAILED) {
        perror("blank_framebuffer_pixels: mmap");
        return;
    }

    // Covers both single- and double-buffered devices (smem_len spans all
    // panning buffers when double-buffered), and works regardless of bpp
    // since all-zero bytes is black in every pixel format this driver is
    // expected to see (RGB565/RGB888/XRGB8888/etc all encode black as 0).
    memset(mem, 0, screensize);

    munmap(mem, screensize);
    fprintf(stderr, "Framebuffer pixels cleared to black (%zu bytes)\n", screensize);
}

bool power_manager::find_backlight()
{
    if (device_config_.configured()) {
        int fd = open(device_config_.backlight_path.c_str(), O_WRONLY);
        if (fd < 0) {
            perror("device_config backlight_path: open");
        } else {
            close(fd);
            backlight_path_ = device_config_.backlight_path;
            max_brightness_ = device_config_.max_brightness > 0 ? device_config_.max_brightness : 255;
            fprintf(stderr, "Using configured backlight: %s (max %d)\n",
                    backlight_path_.c_str(), max_brightness_);
            return true;
        }
        fprintf(stderr, "Configured backlight_path unusable, falling back to auto-detect\n");
    }

    // Auto-detect fallback for devices without a device.conf. Real panels
    // show up under either the generic backlight class or (commonly on
    // MediaTek boards) as a named LED-class device -- try both.
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

            char max_path[160];
            char brightness_path[160];
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
            fprintf(stderr, "Using auto-detected backlight: %s (max %d)\n", brightness_path, max_val);
            closedir(dir);
            return true;
        }

        closedir(dir);
    }

    return false;
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
    if (!set_display_blank(true))
        fprintf(stderr, "Warning: fb blank request failed, continuing in backlight-off sleep\n");

    /* set_display_blank() is a no-op on most panels (screen_blank_supported
     * defaults to false -- see its comment), so the LCD's last frame is
     * typically still latched even though the backlight above has already
     * faded to 0. Clear the actual pixel memory too so the panel is truly
     * black rather than just unlit; this is a direct mmap write, not
     * routed through LVGL, so it's safe to do unconditionally here even
     * when set_display_blank() above did succeed. */
    blank_framebuffer_pixels();

    current_state_ = power_state::sleep;
    fprintf(stderr, "Display turned OFF (sleep)\n");
}

void power_manager::set_touch_indev(lv_indev_t *indev)
{
    touch_indev_ = indev;

    // Apply immediately so this is safe to call at any point, including
    // after the display is already asleep (e.g. touch node hot-plugged).
    set_touch_enabled(current_state_ != power_state::sleep);
}

void power_manager::set_touch_enabled(bool enabled)
{
    if (touch_indev_ == nullptr)
        return;

    lv_indev_enable(touch_indev_, enabled);
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

void power_manager::init(lv_display_t *disp, const device_config_t &config)
{
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
        fprintf(stderr, "No controllable backlight found, sleep/dim will have no visible effect\n");
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

    fprintf(stderr, "Power management initialized (screen_blank_supported=%s)\n",
            device_config_.screen_blank_supported ? "true" : "false");
}

bool power_manager::set_state(power_state state)
{
    if (!initialized_)
        return false;

    if (state == current_state_)
        return true;

    switch (state) {
        case power_state::on: {
            if (!set_display_blank(false))
                fprintf(stderr, "Warning: fb unblank request failed, backlight restored anyway\n");

            current_state_ = power_state::on;
            fprintf(stderr, "Display turned ON\n");
            set_touch_enabled(true);
            cancel_dim_hold();
            if (fade_timer_ != nullptr) {
                lv_timer_delete(fade_timer_);
                fade_timer_ = nullptr;
            }
            fade_complete_cb_ = nullptr;
            lv_obj_invalidate(lv_screen_active());
            /* Do NOT bump the backlight or call lv_timer_handler()/
             * lv_refr_now() here. set_state() can be reached from
             * toggle_sleep() -> on_input_event(), which is itself invoked
             * from input_monitor_.poll() inside activity_timer_trampoline()
             * -- an LVGL timer callback. Calling into LVGL's redraw/timer
             * machinery re-entrantly from inside a running timer callback is
             * unsafe: it corrupts LVGL's internal timer list bookkeeping
             * (including our own fade_timer_/dim_hold_timer_).
             *
             * The panel was just cleared to black by a direct mmap write
             * (blank_framebuffer_pixels(), bypassing LVGL entirely). If we
             * restored the backlight now and let the real repaint trickle
             * in over later ticks, the panel would visibly show that black
             * frame -- or a partially-composited one, since LVGL's default
             * partial-render mode can flush the label/button's own
             * invalidated area before the rest of the background catches up
             * -- while already lit. That's the glitchy "square" around the
             * widgets on wake.
             *
             * Instead, flag that a forced full redraw AND the backlight
             * restore are both owed once we're back at the top level, and
             * do the redraw first: compose the complete correct frame into
             * the (still dark) panel via lv_refr_now(), THEN raise the
             * backlight. The panel then never shows anything but the final
             * frame -- there's no partial state to be caught mid-composite
             * regardless of how LVGL batches its flushes. See
             * service_pending_wake().
             */
            pending_full_redraw_ = true;
            break;
        }

        case power_state::dimmed: {
            /* Panel stays interactive: only the backlight fades down to
             * half of whatever it currently is, then holds there for
             * kDimHoldMs before escalating to sleep on its own. Touch stays
             * enabled here -- dimmed is still "awake", just warning the
             * user before a real sleep. */
            current_state_ = power_state::dimmed;
            fprintf(stderr, "Display DIMMED\n");
            int current = read_current_brightness_percent();
            fade_brightness_to(current / 4);
            cancel_dim_hold();
            dim_hold_timer_ = lv_timer_create(dim_hold_timer_trampoline, kDimHoldMs, this);
            lv_timer_set_repeat_count(dim_hold_timer_, 1);
            break;
        }

        case power_state::sleep:
            cancel_dim_hold();
            /* Commit the state change (including disabling touch) synchronously
             * so toggle_sleep() sees the correct state immediately and no
             * stray taps land during the ~150ms fade-to-black below. Leaving
             * current_state_ set only inside finish_sleep_after_fade() created
             * a window where a second power-button press during the fade
             * would read current_state_ as still "on" and re-enter this case,
             * corrupting the pending fade sequence. */
            current_state_ = power_state::sleep;
            /* Disabling the touch indev here, not after the fade completes,
             * means a finger already on the glass at the moment sleep is
             * triggered stops generating events immediately, rather than
             * being able to land one more accidental tap during the fade. */
            set_touch_enabled(false);
            /* Fade whatever backlight is left down to black, then blank the
             * panel (if this device supports it) only once that fade
             * completes, instead of cutting straight to off. */
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

bool power_manager::service_pending_wake()
{
    if (!pending_full_redraw_)
        return false;

    pending_full_redraw_ = false;

    /* Compose the full correct frame into the framebuffer while the panel
     * is still dark (backlight was left at 0 by finish_sleep_after_fade();
     * set_state(on) deliberately did not touch it -- see its comment).
     * Safe to call lv_refr_now() here: we're at the top level in the main
     * loop, not inside any LVGL timer callback. */
    if (disp_ != nullptr)
        lv_refr_now(disp_);

    /* Only now bring the backlight back up, once a complete frame is
     * already sitting in the framebuffer -- so the panel goes straight
     * from black to the final correct image, with nothing partial or
     * stale ever visible in between. */
    write_brightness_percent(100);

    return true;
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
