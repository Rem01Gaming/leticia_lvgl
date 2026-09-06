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

/**
 * @brief Resolves a real on-disk path to a (possibly binary) resource,
 * using the same lookup tiers as resolve_config_text().
 *
 * Unlike resolve_config_text(), this does not read the resource into
 * memory: it is meant for files an on-disk path is needed for (fonts
 * opened by FreeType via fopen(), for example) or that are too large for
 * the text resolver's size cap. When the resource is a sibling file it is
 * used directly; when it must be extracted from the zip, it is written to
 * a temp file under out_extracted_path.
 *
 * @param zip_path Path to the OTA zip file.
 * @param env_var Environment variable name for override.
 * @param sibling_suffix Suffix for the sibling resource file.
 * @param zip_entry Entry name inside the zip file.
 * @param log_label Label used for logging.
 * @param out_resolved_path Path to the resolved resource on disk.
 * @return true if resolved successfully, false otherwise.
 */
bool resolve_config_file_path(const std::string &zip_path, const char *env_var, const std::string &sibling_suffix, const char *zip_entry, const char *log_label, std::string &out_resolved_path);

} // namespace Leticia
