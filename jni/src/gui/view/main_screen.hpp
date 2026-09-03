#pragma once

#include <atomic>

#include "audio/audio_manager.hpp"
#include "gui/components/status_bar.hpp"
#include "power/power_manager.hpp"

namespace Leticia::screens {

/**
 * @brief Builds the sine wave test screen: an audio toggle button and an exit button.
 *
 * @param audio Audio manager driving the test tone button.
 * @param power Power manager notified on every touch to reset the activity timer.
 * @param status_bar Persistent status bar; its height reserves space at the top of this screen.
 * @param should_exit Flag set when the exit button is pressed, watched by the main loop.
 */
void build_main_screen(Leticia::audio_manager &audio, Leticia::power_manager &power,
                        const Leticia::gui::status_bar &status_bar, std::atomic<bool> &should_exit);

} // namespace Leticia::screens
