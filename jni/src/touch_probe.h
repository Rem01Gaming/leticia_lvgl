#pragma once

#include <stddef.h>

#define TOUCH_PROBE_MAX_PATH 288

/**
 * @brief Scan /dev/input/event* and return the path of the first node
 *        that reports multitouch or single touch absolute axes.
 * @param out_path Buffer to receive the discovered device path.
 * @param out_size Size of out_path in bytes.
 * @return 0 on success, -1 if no touch capable node was found.
 */
int touch_probe_find(char *out_path, size_t out_size);
