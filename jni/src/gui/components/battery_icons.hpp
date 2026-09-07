#pragma once

#include <string>

#include "power/battery_monitor.hpp"

namespace Leticia::gui::battery_icons {

/**
 * @brief Resolves the on-disk paths of the battery/charging SVGs.
 *
 * Same resolution tiers as font_manager::init() (env override, then the
 * flashable/svg folder extracted from the OTA zip). Must be called once
 * before icon_path() or bolt_path(), after Leticia::init_resources().
 *
 * @param zip_path Path to the OTA zip file.
 * @return true if every icon resolved, false otherwise.
 */
bool init(const std::string &zip_path);

/**
 * @brief Gets the resolved battery body icon path for a charge state.
 *
 * @param percent Charge percentage, 0 to 100.
 * @param status Current battery status; full returns the full icon,
 * while others return a fill level based on percentage.
 * @return Absolute path to the matching battery_android_*.svg, or an
 * empty string if init() was not called or failed.
 */
const std::string &icon_path(int percent, Leticia::battery_status status);

/**
 * @brief Gets the resolved charging bolt overlay icon path.
 *
 * @return Absolute path to bolt.svg, or an empty string if init() was
 * not called or failed.
 */
const std::string &bolt_path();

} // namespace Leticia::gui::battery_icons
