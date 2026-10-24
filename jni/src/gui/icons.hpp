#pragma once

#include <cstdint>
#include <string>

namespace Leticia::gui::icons {

constexpr uint32_t kWhite = 0xFFFFFF;

/**
 * Gets the LVGL file path of an icon in a given color.
 *
 * LVGL does not recolor SVG images. This function writes a tinted copy of the
 * icon to the resource directory on first use and returns its path.
 *
 * @param name Icon name without the .svg extension.
 * @param rgb Fill color as 0xRRGGBB.
 * @return Path with the LVGL drive prefix. The path of the white icon if the copy fails.
 */
std::string path(const std::string &name, uint32_t rgb);

} // namespace Leticia::gui::icons
