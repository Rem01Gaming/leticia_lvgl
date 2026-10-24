/**
 * @file lv_linux_fbdev.h
 *
 */

#ifndef LV_LINUX_FBDEV_H
#define LV_LINUX_FBDEV_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#include "../../display/lv_display.h"

#if LV_USE_LINUX_FBDEV

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/
lv_display_t * lv_linux_fbdev_create(void);

lv_result_t lv_linux_fbdev_set_file(lv_display_t * disp, const char * file);

/**
 * Force the display to be refreshed on every change.
 * Expected to be used with LV_DISPLAY_RENDER_MODE_DIRECT or LV_DISPLAY_RENDER_MODE_FULL.
 */
void lv_linux_fbdev_set_force_refresh(lv_display_t * disp, bool enabled);

/**
 * Exchange red and blue when copying pixels to the framebuffer (24 and 32 bpp only).
 *
 * lv_linux_fbdev_set_file() already detects this from the channel offsets the kernel reports,
 * so it only needs to be called to override that detection, e.g. when a driver reports the
 * wrong layout. Call it after lv_linux_fbdev_set_file() and before the first refresh.
 *
 * @param disp      the display created by lv_linux_fbdev_create()
 * @param enabled   true to swap red and blue, false to copy pixels unchanged
 */
void lv_linux_fbdev_set_swap_rb(lv_display_t * disp, bool enabled);

/**********************
 *      MACROS
 **********************/

#endif /* LV_USE_LINUX_FBDEV */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LV_LINUX_FBDEV_H */
