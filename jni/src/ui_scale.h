#pragma once

#include <lvgl.h>

/**
 * @brief Estimate the panel's DPI from its pixel resolution, since most
 *        Android fb nodes report zero for the physical width/height fields
 *        in fb_var_screeninfo. Mirrors AROMA's phone dpi fallback.
 * @param hor_res Horizontal resolution in pixels.
 * @param ver_res Vertical resolution in pixels.
 * @return Estimated DPI, clamped to a sane phone panel range.
 */
int ui_scale_estimate_dpi(int32_t hor_res, int32_t ver_res);

/**
 * @brief Scale factor relative to a 160 dpi baseline, the same reference
 *        Android uses for its 1x (mdpi) density bucket.
 * @param dpi Estimated or reported panel DPI.
 * @return Multiplier to apply to any raw pixel size written for a
 *         160 dpi reference so it stays visually proportional on this panel.
 */
float ui_scale_factor(int dpi);

/**
 * @brief Pick the closest enabled Montserrat font to a target pixel size.
 * @param target_px Desired glyph size in pixels.
 * @return Pointer to the closest available built in font.
 */
const lv_font_t *ui_scale_pick_font(int target_px);
