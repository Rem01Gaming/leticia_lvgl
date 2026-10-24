#pragma once

#include <string>
#include <vector>

#include "audio_manager.hpp"

namespace Leticia::audio_library {

/**
 * Collects the supported audio files below a directory, including subdirectories.
 *
 * @param directory Root directory.
 * @param files Receives the file paths.
 */
void collect_audio_files(const std::string &directory, std::vector<std::string> &files);

/**
 * Reads the tags, the duration and the cover of one file.
 *
 * @param path File path.
 * @param cover_size_px Edge length of the square cover, in pixels.
 * @return Track data. The path is empty if the file has no decodable audio.
 */
music_track_t read_track(const std::string &path, unsigned int cover_size_px);

} // namespace Leticia::audio_library
