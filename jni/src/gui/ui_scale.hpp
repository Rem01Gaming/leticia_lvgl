#pragma once

#include <lvgl.h>

namespace Leticia::ui_scale {

/**
 * @brief Estimates the panels DPI from pixel resolution.
 *
 * @param hor_res Horizontal resolution.
 * @param ver_res Vertical resolution.
 * @return Estimated DPI value.
 */
int estimate_dpi(int32_t hor_res, int32_t ver_res);

/**
 * @brief Picks a font closest to the target pixel size.
 *
 * @param target_px Target pixel size.
 * @return Pointer to the selected LVGL font.
 */
const lv_font_t *pick_font(int target_px);

} // namespace Leticia::ui_scale
