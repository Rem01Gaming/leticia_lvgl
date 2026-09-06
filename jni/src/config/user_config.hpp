#pragma once

#include <string>

namespace Leticia {

/**
 * @brief User configuration loaded at runtime.
 */
struct user_config_t {
    /**
     * @brief IANA timezone name, e.g. "America/Sao_Paulo".
     */
    std::string timezone;

    /**
     * @brief Filesystem path scanned for music files.
     */
    std::string music_scan_path;

    /**
     * @brief Checks if the configuration is valid.
     *
     * @return true if valid, false otherwise.
     */
    bool configured() const {
        return !timezone.empty() || !music_scan_path.empty();
    }
};

/**
 * @brief Loads the user configuration.
 *
 * Resolves the configuration path (env override or extracted resource)
 * and parses it.
 *
 * @param zip_path Path to the OTA zip file.
 * @param out Structure to store the configuration.
 * @return true if loaded successfully, false otherwise.
 */
bool load_user_config(const std::string &zip_path, user_config_t &out);

/**
 * @brief Parses user configuration from a file.
 *
 * @param path Path to the configuration file.
 * @param out Structure to store the configuration.
 * @return true if parsed successfully, false otherwise.
 */
bool load_user_config_from_file(const std::string &path, user_config_t &out);

} // namespace Leticia
