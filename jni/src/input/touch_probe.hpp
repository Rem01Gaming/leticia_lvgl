#pragma once

#include <optional>
#include <string>

namespace Leticia::touch_probe {

/**
 * @brief Finds the first available touchscreen input node.
 *
 * @return Path to the device or nullopt if not found.
 */
std::optional<std::string> find();

} // namespace Leticia::touch_probe
