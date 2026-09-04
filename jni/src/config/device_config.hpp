#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Leticia {

/**
 * @brief Where a front-camera cutout sits along the top edge of the screen,
 * if it intrudes into the status bar row.
 *
 * Covers hole-punch and teardrop cameras alike — their exact shape doesn't
 * matter for layout purposes, only the horizontal span to keep clear of
 * status bar content. Side-mounted cameras (e.g. under the bezel, outside
 * the display area) don't intrude on the status bar at all and should use
 * `none`.
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
     * @brief Physical corner radius of the screen glass, in dp. 0 means
     * square corners. Used to keep status bar content clear of rounded
     * corners, which otherwise clip content anchored flush to the top edge.
     */
    int screen_corner_radius_dp = 0;

    /**
     * @brief Where the front camera cutout sits, if it intrudes into the
     * status bar row. Defaults to none (no cutout, or one outside the
     * display area such as a side-mounted camera).
     */
    cutout_position camera_cutout = cutout_position::none;

    /**
     * @brief Horizontal width of the camera cutout's safe zone, in dp.
     * Ignored when camera_cutout is none. This is the span to keep clear
     * of content, not just the visible hole/teardrop diameter — include
     * whatever margin the panel needs.
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
 * @param zip_path Path to the OTA zip file.
 * @param out Structure to store the configuration.
 * @return true if loaded successfully, false otherwise.
 */
bool load_device_config(const std::string &zip_path, device_config_t &out);

/**
 * @brief Reads the ALSA UCM configuration text.
 *
 * @param zip_path Path to the OTA zip file.
 * @param out_text String to store the configuration text.
 * @return true if read successfully, false otherwise.
 */
bool load_alsa_ucm_config_text(const std::string &zip_path, std::string &out_text);

} // namespace Leticia
