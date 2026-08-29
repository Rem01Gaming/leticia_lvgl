#pragma once

namespace Leticia::evdev_probe {

/**
 * @brief Tests whether a bit is set in a Linux ioctl capability/state bitmask.
 *
 * @param bitmask Bitmask as returned by an EVIOCGBIT/EVIOCGSW style ioctl.
 * @param bit Bit position to test.
 * @return true if the bit is set, false otherwise.
 */
bool has_bit(const unsigned long *bitmask, unsigned int bit);

/**
 * @brief Checks whether a device node reports a given EV_KEY key code.
 *
 * @param fd Open file descriptor for the device node.
 * @param key_code Linux input key code (e.g. KEY_POWER).
 * @return true if the device can report that key, false otherwise.
 */
bool has_key(int fd, unsigned int key_code);

/**
 * @brief Checks whether a device node reports a given EV_SW switch code.
 *
 * @param fd Open file descriptor for the device node.
 * @param switch_code Linux input switch code (e.g. SW_HEADPHONE_INSERT).
 * @return true if the device can report that switch, false otherwise.
 */
bool has_switch(int fd, unsigned int switch_code);

/**
 * @brief Checks whether a device node reports a given EV_ABS absolute axis.
 *
 * @param fd Open file descriptor for the device node.
 * @param abs_code Linux input absolute axis code (e.g. ABS_X).
 * @return true if the device can report that axis, false otherwise.
 */
bool has_abs(int fd, unsigned int abs_code);

} // namespace Leticia::evdev_probe
