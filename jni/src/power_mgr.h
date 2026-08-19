#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <lvgl.h>

/**
 * @brief Power management states for the display
 */
typedef enum {
    POWER_STATE_ON,      /**< Display and system fully active */
    POWER_STATE_DIMMED,      /**< Backlight faded down, panel still lit */
    POWER_STATE_SLEEP,       /**< Display off, system idle (music can play) */
    POWER_STATE_DEEP_SLEEP   /**< Deep sleep mode (reserved for future) */
} power_state_t;

/**
 * @brief Initialize the power management subsystem
 * @param disp Pointer to the LVGL display object
 */
void power_mgmt_init(lv_display_t *disp);

/**
 * @brief Set the current power state
 * @param state The desired power state
 * @return 0 on success, -1 on failure
 */
int power_mgmt_set_state(power_state_t state);

/**
 * @brief Get the current power state
 * @return Current power state
 */
power_state_t power_mgmt_get_state(void);

/**
 * @brief Toggle between ON and SLEEP states
 * @return The new power state
 */
power_state_t power_mgmt_toggle_sleep(void);

/**
 * @brief Set the inactivity timeout before auto-sleep (in seconds)
 * @param timeout_seconds Timeout in seconds (0 to disable auto-sleep).
 *        The screen dims briefly as a warning shortly before this fires,
 *        the same way phones and recovery UIs do.
 */
void power_mgmt_set_sleep_timeout(uint32_t timeout_seconds);

/**
 * @brief Reset the inactivity timer (call on user interaction)
 */
void power_mgmt_reset_activity_timer(void);

/**
 * @brief Enable or disable the power button handler
 * @param enabled true to enable, false to disable
 */
void power_mgmt_set_pwr_button_enabled(bool enabled);

/**
 * @brief Cleanup power management resources
 */
void power_mgmt_deinit(void);
