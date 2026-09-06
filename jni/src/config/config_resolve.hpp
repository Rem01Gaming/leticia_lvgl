#pragma once

#include <string>

namespace Leticia {

/**
 * @brief Prepares the temporary resource directory by extracting assets
 * from the OTA zip.
 *
 * This nukes /tmp/leticia/ and re-extracts the 'config', 'fonts', and 'svg'
 * folders to ensure fresh assets. Must be called before any resolve_* function.
 *
 * @param zip_path Path to the OTA zip file.
 * @return true if successful or if no zip_path is provided (clean slate).
 */
bool init_resources(const std::string &zip_path);

/**
 * @brief Resolves a real on-disk path to a (possibly binary) resource.
 *
 * Lookup order: environment variable override, then the extracted file
 * in /tmp/leticia/.
 *
 * @param zip_path Path to the OTA zip file (used for logging context).
 * @param env_var Environment variable name for override.
 * @param zip_entry Entry name inside the zip file.
 * @param log_label Label used for logging.
 * @param out_resolved_path Path to the resolved resource on disk.
 * @return true if resolved successfully, false otherwise.
 */
bool resolve_config_file_path(const std::string &zip_path, const char *env_var, const char *zip_entry, const char *log_label, std::string &out_resolved_path);

} // namespace Leticia
