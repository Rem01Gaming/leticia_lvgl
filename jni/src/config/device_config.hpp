#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Leticia {

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
