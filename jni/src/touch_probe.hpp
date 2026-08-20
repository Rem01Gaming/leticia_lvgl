#pragma once

#include <optional>
#include <string>

namespace Leticia::TouchProbe {

/**
 * @brief Scan /dev/input/event* and return the path of the first node
 *        that reports multitouch or single touch absolute axes.
 * @return The device path, or std::nullopt if none was found.
 */
std::optional<std::string> find();

} // namespace Leticia::TouchProbe
