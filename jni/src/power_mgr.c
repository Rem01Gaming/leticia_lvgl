#include "power_mgr.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <lvgl.h>

// Default sleep timeout in seconds
#define DEFAULT_SLEEP_TIMEOUT_SEC 30

// Default dim timeout in seconds
#define DEFAULT_DIM_TIMEOUT_SEC 20

// How long the screen stays dimmed before escalating to sleep
#define DIM_HOLD_MS 5000

// Time that takes to fade the backlight to blank/off
#define BRIGHTNESS_FADE_MS 150

// Time that takes to fade the backlight to intermediate level
#define BRIGHTNESS_FADE_STEP_MS 15

/* Helper to test a bit in a bitmask */
static inline int test_bit(unsigned int bit, const unsigned long *bitmask)
{
    return (bitmask[bit / (sizeof(unsigned long) * 8)] >> (bit % (sizeof(unsigned long) * 8))) & 1;
}

typedef struct {
    lv_display_t *disp;
    int fb_fd;                          /* File descriptor for framebuffer */
    char fb_path[128];                  /* Path to framebuffer device */
    int pwr_btn_fd;                     /* File descriptor for power button */
    power_state_t current_state;
    uint32_t sleep_timeout_sec;
    uint32_t dim_timeout_sec;
    uint64_t last_activity_time_ms;
    bool pwr_button_enabled;
    bool initialized;
    lv_timer_t *activity_timer;         /* Timer to check for inactivity */

    char backlight_path[128];           /* sysfs brightness file, empty if none found */
    int max_brightness;                 /* value read from max_brightness, 255 if unknown */
    int brightness_percent;             /* last brightness value actually written, 0..100 */
    lv_timer_t *fade_timer;             /* drives the soft dim/restore ramp */
    int fade_from_percent;
    int fade_to_percent;
    uint64_t fade_start_ms;
    lv_timer_t *dim_hold_timer;         /* fires once, DIM_HOLD_MS after entering DIMMED */
} power_mgmt_ctx_t;

static power_mgmt_ctx_t g_ctx = {0};

/**
 * @brief Get current time in milliseconds
 */
static uint64_t get_current_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (ts.tv_nsec / 1000000);
}

/**
 * @brief Blank/unblank via sysfs (fallback for many Android devices)
 * @param blank true to blank, false to unblank
 * @return 0 on success, -1 on failure
 */
static int set_display_blank_sysfs(bool blank)
{
    const char *sysfs_paths[] = {
        "/sys/class/graphics/fb0/blank",
        "/sys/class/graphics/fb1/blank",
        NULL
    };

    for (const char **path = sysfs_paths; *path != NULL; path++) {
        int fd = open(*path, O_WRONLY);
        if (fd < 0)
            continue;

        /* Value: 0 = on, 1 = off (or 4 for powerdown) */
        const char *val = blank ? "1" : "0";
        ssize_t ret = write(fd, val, strlen(val));
        close(fd);
        if (ret > 0) {
            fprintf(stderr, "sysfs blanking %s via %s\n", blank ? "OFF" : "ON", *path);
            return 0;
        }
    }
    return -1;
}

/**
 * @brief Blank or unblank the display using ioctl first, then sysfs fallback
 * @param blank true to blank (turn off), false to unblank
 * @return 0 on success, -1 on failure
 */
static int set_display_blank(bool blank)
{
    /* Try ioctl on the framebuffer device first */
    if (g_ctx.fb_fd >= 0) {
        int blank_mode = blank ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK;
        if (ioctl(g_ctx.fb_fd, FBIOBLANK, blank_mode) == 0) {
            fprintf(stderr, "ioctl blanking %s\n", blank ? "OFF" : "ON");
            return 0;
        }
        perror("ioctl(FBIOBLANK)");
    }

    /* Fallback to sysfs */
    if (set_display_blank_sysfs(blank) == 0)
        return 0;

    fprintf(stderr, "All blanking methods failed\n");
    return -1;
}

/**
 * @brief Find the first usable backlight under /sys/class/backlight and
 *        cache its brightness path and max_brightness value.
 * @return true if a controllable backlight was found.
 */
static bool find_backlight(void)
{
    DIR *dir = opendir("/sys/class/backlight");
    if (dir == NULL)
        return false;

    struct dirent *entry;
    bool found = false;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;

        char max_path[160];
        char brightness_path[160];
        snprintf(max_path, sizeof(max_path), "/sys/class/backlight/%s/max_brightness", entry->d_name);
        snprintf(brightness_path, sizeof(brightness_path), "/sys/class/backlight/%s/brightness", entry->d_name);

        FILE *f = fopen(max_path, "r");
        if (f == NULL)
            continue;

        int max_val = 0;
        int scanned = fscanf(f, "%d", &max_val);
        fclose(f);

        if (scanned != 1 || max_val <= 0)
            continue;

        /* Confirm brightness is actually writable before committing to this node */
        int fd = open(brightness_path, O_WRONLY);
        if (fd < 0)
            continue;
        close(fd);

        strncpy(g_ctx.backlight_path, brightness_path, sizeof(g_ctx.backlight_path) - 1);
        g_ctx.backlight_path[sizeof(g_ctx.backlight_path) - 1] = '\0';
        g_ctx.max_brightness = max_val;
        found = true;
        fprintf(stderr, "Using backlight: %s (max %d)\n", brightness_path, max_val);
        break;
    }

    closedir(dir);
    return found;
}

/**
 * @brief Read current backlight brightness.
 * @return Percentage or the last known cached value if the read fails.
 */
static int read_current_brightness_percent(void)
{
    if (g_ctx.backlight_path[0] == '\0')
        return g_ctx.brightness_percent;

    FILE *f = fopen(g_ctx.backlight_path, "r");
    if (f == NULL)
        return g_ctx.brightness_percent;

    int raw = 0;
    int scanned = fscanf(f, "%d", &raw);
    fclose(f);

    if (scanned != 1 || g_ctx.max_brightness <= 0)
        return g_ctx.brightness_percent;

    int percent = (raw * 100) / g_ctx.max_brightness;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    g_ctx.brightness_percent = percent;
    return percent;
}

/**
 * @brief Write a raw brightness percentage to the backlight, no fade.
 * @param percent 0..100
 */
static void write_brightness_percent(int percent)
{
    if (g_ctx.backlight_path[0] == '\0')
        return;

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    int raw = (percent * g_ctx.max_brightness) / 100;

    int fd = open(g_ctx.backlight_path, O_WRONLY);
    if (fd < 0)
        return;

    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d", raw);
    write(fd, buf, (size_t)len);
    close(fd);

    g_ctx.brightness_percent = percent;
}

typedef void (*fade_complete_cb_t)(void);
static fade_complete_cb_t g_fade_complete_cb = NULL;

/**
 * @brief Timer callback that ramps brightness_percent between
 *        fade_from_percent and fade_to_percent over BRIGHTNESS_FADE_MS.
 */
static void brightness_fade_timer_cb(lv_timer_t *timer)
{
    uint64_t elapsed = get_current_time_ms() - g_ctx.fade_start_ms;

    if (elapsed >= BRIGHTNESS_FADE_MS) {
        write_brightness_percent(g_ctx.fade_to_percent);
        lv_timer_delete(timer);
        g_ctx.fade_timer = NULL;

        fade_complete_cb_t cb = g_fade_complete_cb;
        g_fade_complete_cb = NULL;
        if (cb != NULL)
            cb();
        return;
    }

    int delta = g_ctx.fade_to_percent - g_ctx.fade_from_percent;
    int percent = g_ctx.fade_from_percent + (int)((delta * (int64_t)elapsed) / BRIGHTNESS_FADE_MS);
    write_brightness_percent(percent);
}

/**
 * @brief Start (or redirect) a soft brightness ramp toward target_percent.
 *        No-op if no backlight was found; falls back to an instant jump
 *        by the caller in that case.
 * @param on_complete Optional callback fired once the target is reached.
 *        Not called if there's no backlight (fade is a no-op) or if the
 *        current brightness already matches the target.
 */
static void fade_brightness_to(int target_percent, fade_complete_cb_t on_complete)
{
    if (g_ctx.backlight_path[0] == '\0') {
        if (on_complete != NULL)
            on_complete();
        return;
    }

    if (g_ctx.fade_timer != NULL) {
        lv_timer_delete(g_ctx.fade_timer);
        g_ctx.fade_timer = NULL;
    }

    g_ctx.fade_from_percent = read_current_brightness_percent();
    g_ctx.fade_to_percent = target_percent;
    g_ctx.fade_start_ms = get_current_time_ms();
    g_fade_complete_cb = on_complete;

    if (g_ctx.fade_from_percent == g_ctx.fade_to_percent) {
        g_fade_complete_cb = NULL;
        if (on_complete != NULL)
            on_complete();
        return;
    }

    g_ctx.fade_timer = lv_timer_create(brightness_fade_timer_cb, BRIGHTNESS_FADE_STEP_MS, NULL);
}

/**
 * @brief Cancel the pending dim-to-sleep escalation, if any.
 */
static void cancel_dim_hold(void)
{
    if (g_ctx.dim_hold_timer != NULL) {
        lv_timer_delete(g_ctx.dim_hold_timer);
        g_ctx.dim_hold_timer = NULL;
    }
}

/**
 * @brief One-shot callback, fired DIM_HOLD_MS after entering DIMMED.
 *        Escalates straight to SLEEP, same as the real thing. The timer
 *        itself is repeat_count=1 and LVGL deletes it after this returns.
 */
static void dim_hold_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    g_ctx.dim_hold_timer = NULL;

    if (g_ctx.current_state == POWER_STATE_DIMMED)
        power_mgmt_set_state(POWER_STATE_SLEEP);
}

/**
 * @brief Blanks the panel once the fade-to-black finishes. Runs as the
 *        completion callback for the SLEEP transition's fade.
 */
static void finish_sleep_after_fade(void)
{
    if (set_display_blank(true) == 0) {
        g_ctx.current_state = POWER_STATE_SLEEP;
        fprintf(stderr, "Display turned OFF (sleep)\n");
    } else {
        fprintf(stderr, "Failed to blank display after fade\n");
    }
}

/**
 * @brief Open the first input device that reports KEY_POWER
 * @return File descriptor or -1 on failure
 */
static int open_power_button(void)
{
    /* Check environment override first */
    const char *env_override = getenv("PWR_BUTTON_DEVICE");
    if (env_override != NULL) {
        int fd = open(env_override, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            fprintf(stderr, "Using power button (env): %s\n", env_override);
            return fd;
        }
    }

    DIR *dir = opendir("/dev/input");
    if (dir == NULL) {
        fprintf(stderr, "Cannot open /dev/input\n");
        return -1;
    }

    struct dirent *entry;
    int found_fd = -1;

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0)
            continue;

        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;

        /* Check if this device supports EV_KEY and KEY_POWER */
        unsigned long ev_bits[((EV_MAX + (sizeof(unsigned long) * 8) - 1) /
                               (sizeof(unsigned long) * 8))] = {0};
        unsigned long key_bits[((KEY_MAX + (sizeof(unsigned long) * 8) - 1) /
                                (sizeof(unsigned long) * 8))] = {0};

        if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
            close(fd);
            continue;
        }
        if (!test_bit(EV_KEY, ev_bits)) {
            close(fd);
            continue;
        }

        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
            close(fd);
            continue;
        }
        if (!test_bit(KEY_POWER, key_bits)) {
            close(fd);
            continue;
        }

        /* Found it */
        found_fd = fd;
        fprintf(stderr, "Using power button: %s\n", path);
        break;
    }

    closedir(dir);

    if (found_fd < 0) {
        fprintf(stderr, "No power button found, button control disabled\n");
    }
    return found_fd;
}

/**
 * @brief Timer callback to check for inactivity and power button
 */
static void activity_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!g_ctx.initialized || !g_ctx.pwr_button_enabled)
        return;

    if (g_ctx.pwr_btn_fd >= 0) {
        struct input_event ev;
        ssize_t n;
        while ((n = read(g_ctx.pwr_btn_fd, &ev, sizeof(ev))) == sizeof(ev)) {
            if (ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 1) {
                power_mgmt_toggle_sleep();
                power_mgmt_reset_activity_timer();
            }
        }
        if (n < 0 && errno != EAGAIN) {
            static bool logged = false;
            if (!logged) {
                perror("read power button");
                logged = true;
            }
        }
    }

    if (g_ctx.sleep_timeout_sec == 0 && g_ctx.dim_timeout_sec == 0)
        return;

    uint64_t now = get_current_time_ms();
    uint64_t idle_sec = (now - g_ctx.last_activity_time_ms) / 1000;

    if (g_ctx.current_state == POWER_STATE_ON) {
        bool dim_enabled = g_ctx.dim_timeout_sec > 0 &&
                            g_ctx.dim_timeout_sec < g_ctx.sleep_timeout_sec;

        if (dim_enabled && idle_sec >= g_ctx.dim_timeout_sec) {
            fprintf(stderr, "Auto-dim triggered after %lu seconds of inactivity\n",
                    (unsigned long)g_ctx.dim_timeout_sec);
            power_mgmt_set_state(POWER_STATE_DIMMED);
        } else if (!dim_enabled && g_ctx.sleep_timeout_sec > 0 &&
                   idle_sec >= g_ctx.sleep_timeout_sec) {
            fprintf(stderr, "Auto-sleep triggered after %lu seconds of inactivity\n",
                    (unsigned long)g_ctx.sleep_timeout_sec);
            power_mgmt_set_state(POWER_STATE_SLEEP);
        }
    }
    /* Once DIMMED, the dim_hold_timer owns escalation to SLEEP on its
     * own fixed delay, independent of sleep_timeout_sec. */
}

/**
 * @brief Initialize the power management subsystem
 */
void power_mgmt_init(lv_display_t *disp)
{
    if (g_ctx.initialized) {
        return;
    }

    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.disp = disp;
    g_ctx.current_state = POWER_STATE_ON;
    g_ctx.sleep_timeout_sec = DEFAULT_SLEEP_TIMEOUT_SEC;
    g_ctx.dim_timeout_sec = DEFAULT_DIM_TIMEOUT_SEC;
    g_ctx.pwr_button_enabled = true;
    g_ctx.last_activity_time_ms = get_current_time_ms();
    g_ctx.brightness_percent = 100;
    g_ctx.max_brightness = 255;

    if (find_backlight()) {
        write_brightness_percent(100);
    } else {
        fprintf(stderr, "No controllable backlight found, dim will fall back to blanking\n");
    }

    /* Determine framebuffer path (same logic as LVGL) */
    const char *fb_path = getenv("FB_DEVICE");
    if (fb_path == NULL) {
        if (access("/dev/graphics/fb0", F_OK) == 0)
            fb_path = "/dev/graphics/fb0";
        else if (access("/dev/fb0", F_OK) == 0)
            fb_path = "/dev/fb0";
        else
            fb_path = "/dev/fb0"; /* fallback */
    }
    strncpy(g_ctx.fb_path, fb_path, sizeof(g_ctx.fb_path) - 1);
    g_ctx.fb_path[sizeof(g_ctx.fb_path) - 1] = '\0';

    /* Open framebuffer for blanking ioctl */
    g_ctx.fb_fd = open(g_ctx.fb_path, O_RDWR);
    if (g_ctx.fb_fd < 0) {
        fprintf(stderr, "Warning: Could not open framebuffer %s for power control\n", g_ctx.fb_path);
        /* Continue without direct ioctl, sysfs fallback will be attempted */
    } else {
        fprintf(stderr, "Opened framebuffer %s for power control\n", g_ctx.fb_path);
    }

    /* Open power button */
    g_ctx.pwr_btn_fd = open_power_button();

    /* Create timer for checking activity and power button */
    g_ctx.activity_timer = lv_timer_create(activity_timer_cb, 500, NULL);

    g_ctx.initialized = true;

    fprintf(stderr, "Power management initialized\n");
}

/**
 * @brief Set the current power state
 */
int power_mgmt_set_state(power_state_t state)
{
    if (!g_ctx.initialized) {
        return -1;
    }

    if (state == g_ctx.current_state) {
        return 0;
    }

    int ret = 0;

    switch (state) {
        case POWER_STATE_ON:
            ret = set_display_blank(false);
            if (ret == 0) {
                g_ctx.current_state = POWER_STATE_ON;
                fprintf(stderr, "Display turned ON\n");
                cancel_dim_hold();
                if (g_ctx.fade_timer != NULL) {
                    lv_timer_delete(g_ctx.fade_timer);
                    g_ctx.fade_timer = NULL;
                }
                g_fade_complete_cb = NULL;
                /* Wake is instant, no fade-in, matches real hardware */
                write_brightness_percent(100);
                /* Force LVGL to redraw the screen */
                lv_obj_invalidate(lv_screen_active());
                /* Also trigger a timer run to flush */
                lv_timer_handler();
            } else {
                fprintf(stderr, "Failed to unblank display, state unchanged\n");
                return -1;
            }
            break;

        case POWER_STATE_DIMMED: {
            /* Panel stays unblanked, only the backlight fades down to
             * half of whatever it currently is, then holds there for
             * DIM_HOLD_MS before escalating to sleep on its own. */
            g_ctx.current_state = POWER_STATE_DIMMED;
            fprintf(stderr, "Display DIMMED\n");
            int current = read_current_brightness_percent();
            int half = current / 2;
            fade_brightness_to(half, NULL);
            cancel_dim_hold();
            g_ctx.dim_hold_timer = lv_timer_create(dim_hold_timer_cb, DIM_HOLD_MS, NULL);
            lv_timer_set_repeat_count(g_ctx.dim_hold_timer, 1);
            break;
        }

        case POWER_STATE_SLEEP:
            cancel_dim_hold();
            /* Fade whatever backlight is left down to black, then blank
             * the panel only once that fade completes, instead of
             * cutting straight to off. */
            fade_brightness_to(0, finish_sleep_after_fade);
            return 0;

        case POWER_STATE_DEEP_SLEEP:
            fprintf(stderr, "Deep sleep not implemented\n");
            return -1;

        default:
            return -1;
    }

    /* Only reset the idle clock when waking to fully ON. Resetting it on
     * DIMMED or SLEEP would restart the countdown and the screen would
     * never progress from dimmed to off on its own. */
    if (state == POWER_STATE_ON)
        power_mgmt_reset_activity_timer();

    return 0;
}

/**
 * @brief Get the current power state
 */
power_state_t power_mgmt_get_state(void)
{
    return g_ctx.current_state;
}

/**
 * @brief Toggle between ON and SLEEP states
 */
power_state_t power_mgmt_toggle_sleep(void)
{
    if (!g_ctx.initialized) {
        return g_ctx.current_state;
    }

    bool currently_awake = (g_ctx.current_state == POWER_STATE_ON) ||
                            (g_ctx.current_state == POWER_STATE_DIMMED);
    power_state_t new_state = currently_awake ? POWER_STATE_SLEEP : POWER_STATE_ON;

    power_mgmt_set_state(new_state);
    power_mgmt_reset_activity_timer();

    return new_state;
}

/**
 * @brief Set the inactivity timeout before the screen dims
 */
void power_mgmt_set_dim_timeout(uint32_t timeout_seconds)
{
    g_ctx.dim_timeout_sec = timeout_seconds;
    fprintf(stderr, "Dim timeout set to %u seconds\n", timeout_seconds);
}

/**
 * @brief Set the inactivity timeout before auto-sleep
 */
void power_mgmt_set_sleep_timeout(uint32_t timeout_seconds)
{
    g_ctx.sleep_timeout_sec = timeout_seconds;
    fprintf(stderr, "Sleep timeout set to %u seconds\n", timeout_seconds);
}

/**
 * @brief Reset the inactivity timer
 */
void power_mgmt_reset_activity_timer(void)
{
    g_ctx.last_activity_time_ms = get_current_time_ms();
}

/**
 * @brief Enable or disable the power button handler
 */
void power_mgmt_set_pwr_button_enabled(bool enabled)
{
    g_ctx.pwr_button_enabled = enabled;
    fprintf(stderr, "Power button %s\n", enabled ? "enabled" : "disabled");
}

/**
 * @brief Cleanup power management resources
 */
void power_mgmt_deinit(void)
{
    if (!g_ctx.initialized) {
        return;
    }

    if (g_ctx.activity_timer != NULL) {
        lv_timer_delete(g_ctx.activity_timer);
        g_ctx.activity_timer = NULL;
    }

    if (g_ctx.fade_timer != NULL) {
        lv_timer_delete(g_ctx.fade_timer);
        g_ctx.fade_timer = NULL;
    }
    g_fade_complete_cb = NULL;

    if (g_ctx.dim_hold_timer != NULL) {
        lv_timer_delete(g_ctx.dim_hold_timer);
        g_ctx.dim_hold_timer = NULL;
    }

    if (g_ctx.pwr_btn_fd >= 0) {
        close(g_ctx.pwr_btn_fd);
        g_ctx.pwr_btn_fd = -1;
    }

    if (g_ctx.fb_fd >= 0) {
        close(g_ctx.fb_fd);
        g_ctx.fb_fd = -1;
    }

    g_ctx.initialized = false;
    fprintf(stderr, "Power management deinitialized\n");
}
