#pragma once

#include <string>

namespace Leticia {

/**
 * @brief Resolves configuration text using multiple lookup tiers.
 *
 * Lookup order: environment variable override, sibling file next to the
 * OTA zip, then an entry inside the OTA zip itself.
 *
 * @param zip_path Path to the OTA zip file.
 * @param env_var Environment variable name for override.
 * @param sibling_suffix Suffix for the sibling configuration file.
 * @param zip_entry Entry name inside the zip file.
 * @param log_label Label used for logging.
 * @param out_text String to store the resolved text.
 * @return true if resolved successfully, false otherwise.
 */
bool resolve_config_text(const std::string &zip_path, const char *env_var, const std::string &sibling_suffix, const char *zip_entry, const char *log_label, std::string &out_text);

} // namespace Leticia
