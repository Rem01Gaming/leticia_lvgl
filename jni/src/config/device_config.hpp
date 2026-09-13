#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Leticia {

/**
 * @brief Where a front-camera cutout sits along the top edge of the screen,
 * if it intrudes into the status bar row.
 */
enum class cutout_position {
    none,       ///< No cutout intrudes into the status bar row.
    top_center, ///< Centered hole-punch or teardrop camera.
    top_left,   ///< Camera offset toward the left of the top edge.
    top_right,  ///< Camera offset toward the right of the top edge.
};

/**
 * @brief Device configuration loaded at runtime.
 */
struct device_config_t {
    /**
     * @brief Path to the backlight sysfs node.
     */
    std::string backlight_path;

    /**
     * @brief Maximum brightness value.
     */
    int max_brightness = 0;

    /**
     * @brief Whether hardware screen blanking is supported.
     */
    bool screen_blank_supported = false;

    /**
     * @brief ALSA card index for audio output.
     */
    unsigned int alsa_card = 0;

    /**
     * @brief Enables ModernAlsa's reopen-before-reconfigure workaround for
     * onboard playback (see pcm::set_reopen_before_reconfigure()).
     */
    bool onboard_audio_reopen_workaround = false;

    /**
     * @brief Caps the PCM sample format offered to the onboard codec.
     */
    unsigned int audio_max_bit_depth = 0;

    /**
     * @brief CPU IDs for LVGL thread pinning.
     */
    std::vector<int> lvgl_thread_affinity;

    /**
     * @brief CPU IDs for Audio thread pinning.
     */
    std::vector<int> audio_thread_affinity;

    /**
     * @brief Path to the battery power_supply sysfs node. Empty means auto-detect.
     */
    std::string battery_path;

    /**
     * @brief Status bar height in dp, reserved at the top of every screen.
     */
    int status_bar_height_dp = 24;

    /**
     * @brief Physical corner radius of the screen glass.
     */
    int screen_corner_radius_dp = 0;

    /**
     * @brief Where the front camera cutout sits.
     */
    cutout_position camera_cutout = cutout_position::none;

    /**
     * @brief Horizontal width of the camera cutout's safe zone.
     */
    int camera_cutout_width_dp = 0;

    /**
     * @brief Checks if the configuration is valid.
     *
     * @return true if valid, false otherwise.
     */
    bool configured() const {
        return !backlight_path.empty();
    }
};

/**
 * @brief Loads the device configuration.
 *
 * Resolves the configuration path (env override or extracted resource)
 * and parses it.
 *
 * @param zip_path Path to the OTA zip file.
 * @param out Structure to store the configuration.
 * @return true if loaded successfully, false otherwise.
 */
bool load_device_config(const std::string &zip_path, device_config_t &out);

/**
 * @brief Parses device configuration from a file.
 *
 * @param path Path to the configuration file.
 * @param out Structure to store the configuration.
 * @return true if parsed successfully, false otherwise.
 */
bool load_device_config_from_file(const std::string &path, device_config_t &out);

/**
 * @brief Resolves the path to the ALSA UCM configuration file.
 *
 * @param zip_path Path to the OTA zip file.
 * @param out_path String to store the resolved path.
 * @return true if resolved successfully, false otherwise.
 */
bool resolve_alsa_ucm_config_path(const std::string &zip_path, std::string &out_path);

} // namespace Leticia
