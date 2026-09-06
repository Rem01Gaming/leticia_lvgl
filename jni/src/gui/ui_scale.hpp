#pragma once

#include <cstdint>

namespace Leticia::ui_scale {

/**
 * @brief Estimates the panels DPI from pixel resolution.
 *
 * @param hor_res Horizontal resolution.
 * @param ver_res Vertical resolution.
 * @return Estimated DPI value.
 */
int estimate_dpi(int32_t hor_res, int32_t ver_res);

} // namespace Leticia::ui_scale
