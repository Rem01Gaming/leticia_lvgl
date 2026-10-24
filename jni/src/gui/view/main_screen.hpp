#pragma once

#include <atomic>

#include "gui/components/status_bar.hpp"
#include "playback/audio_manager.hpp"

namespace Leticia::screens {

/**
 * Builds the music player screen: header, track list, now playing panel and overlays.
 *
 * @param audio Audio manager that drives playback.
 * @param status_bar Persistent status bar. Its height reserves space at the top.
 * @param should_exit Flag set when the user chooses Exit. The main loop watches it.
 */
void build_main_screen(Leticia::audio_manager &audio, const Leticia::gui::status_bar &status_bar,
                       std::atomic<bool> &should_exit);

/**
 * Gets the pixel size that decoded covers must have.
 *
 * Call after the display density is set.
 *
 * @return Cover edge length in pixels.
 */
unsigned int cover_size_px();

} // namespace Leticia::screens
