#pragma once

#include <cstdint>
#include <string>

namespace Leticia {

/**
 * @brief Per-device quirks that used to be compile-time constants
 *        (a la TWRP's BoardConfig.mk TW_BRIGHTNESS_PATH / TW_MAX_BRIGHTNESS /
 *        TW_NO_SCREEN_BLANK). Loaded at runtime instead, so the same
 *        prebuilt binary works across devices without recompilation --
 *        see load_device_config().
 */
struct device_config_t {
    std::string backlight_path; ///< Empty means "not configured, auto-detect".
    int max_brightness = 0;     ///< 0 means "unknown, auto-detect".

    /**
     * Whether FBIOBLANK / fb0 "blank" sysfs actually powers the panel off
     * on this device. Most MTK mtkfb panels do not; defaults to false
     * (backlight-only sleep) since that degrades safely everywhere, even
     * on devices we've never seen. Only set true for a device once it's
     * been verified to actually blank and reliably recover.
     */
    bool screen_blank_supported = false;

    /**
     * ALSA card index for mixer/pcm open (the "0" in "hw:0,x"). Defaults
     * to 0, the common case; only needs overriding on a board where the
     * onboard codec doesn't enumerate as the first card.
     */
    unsigned int alsa_card = 0;

    bool configured() const { return !backlight_path.empty(); }
};

/**
 * @brief Locate and parse a device config, checked in this order:
 *          1. $LETICIA_DEVICE_CONFIG -- absolute path to a config file.
 *             Overrides everything; meant for development/testing.
 *          2. A plain sibling file next to the zip on disk, named
 *             "<zip_path>.leticia.conf". Easiest thing to hand-edit or
 *             drop onto a device without touching the zip at all.
 *          3. An entry named "config/device.conf" inside the zip at
 *             zip_path, extracted via the system `unzip` binary (fork +
 *             exec with an argv array, not a hand-rolled zip parser). This
 *             is the actual "ship one file per device" mechanism: a new
 *             device just needs its own zip built with a different
 *             embedded device.conf, compressed or not -- `unzip -p`
 *             handles either. Requires `unzip` to be present on the
 *             recovery image's PATH; if it isn't, this source is silently
 *             skipped and runtime auto-detection applies instead.
 *
 *        File format is plain `key = value` lines, '#' comments allowed,
 *        recognized keys: backlight_path, max_brightness,
 *        screen_blank_supported (true/false/1/0), alsa_card.
 *
 * @param zip_path Path to the OTA zip on disk (argv[3] as passed by
 *        recovery to update-binary). May be empty if unavailable, in
 *        which case only the env override is checked.
 * @param out Filled in on success.
 * @return true if any source above was found and parsed. false means
 *         nothing was configured for this device -- callers should fall
 *         back to runtime auto-detection rather than treat this as fatal.
 */
bool load_device_config(const std::string &zip_path, device_config_t &out);

/**
 * @brief Locates the raw text of a UCM config for this device, using the
 *        same lookup tiering as load_device_config() (env override,
 *        sibling file, zip-embedded entry) but a distinct set of names so
 *        the two configs don't collide:
 *          1. $LETICIA_ALSA_CONFIG
 *          2. "<zip_path>.leticia.alsa.conf"
 *          3. "config/alsa.conf" inside the zip (matches
 *             flashable/config/alsa.conf's packed location).
 *        Returns raw text rather than a parsed ucm::config so this header
 *        stays free of a dependency on the audio module; the caller is
 *        expected to hand the text to ucm::config::parse().
 * @param zip_path Path to the OTA zip on disk, may be empty.
 * @param out_text Filled with the config's raw text on success.
 * @return true if any source above was found and read.
 */
bool load_alsa_ucm_config_text(const std::string &zip_path, std::string &out_text);

} // namespace Leticia
