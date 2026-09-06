#pragma once

#include <lvgl.h>

#include <string>

namespace Leticia::font_manager {

/**
 * @brief Font weight, matching the CSS-like scale Google Sans's variable
 * font exposes (400 = regular, up to 700 = bold).
 */
enum class weight : int32_t {
    regular = 400,
    medium = 500,
    semibold = 600,
    bold = 700,
};

/**
 * @brief Loads Google Sans and brings up FreeType.
 *
 * Resolves the upright and italic variable-font TTFs (env override,
 * or extracted from the flashed zip, in that order -- same
 * tiers as Leticia::resolve_config_file_path()). Montserrat is fully compiled
 * out of this build, so there is no bitmap font to fall back to: any
 * failure here is unrecoverable and the caller must abort startup.
 *
 * @param zip_path Path to the OTA zip file.
 * @return true if Google Sans loaded successfully, false otherwise.
 */
bool init(const std::string &zip_path);

/**
 * @brief Deletes every font this module created and shuts down FreeType.
 *
 * Call before lv_display_delete(), since deleted fonts must outlive the
 * draw units that may still reference them mid-frame.
 */
void deinit();

/**
 * @brief Gets a Google Sans font at an exact pixel size.
 *
 * Unlike the old Montserrat table, this does not snap to a fixed set of
 * sizes -- FreeType renders at whatever size is requested. Fonts are
 * cached per (size, weight, italic) combination, so repeated calls with
 * the same parameters are cheap.
 *
 * @param size_px Font size in pixels.
 * @param w Font weight.
 * @param italic Whether to use the italic face.
 * @return Pointer to the font. Never null once init() has succeeded.
 */
const lv_font_t *get_font(int32_t size_px, weight w = weight::regular, bool italic = false);

} // namespace Leticia::font_manager
