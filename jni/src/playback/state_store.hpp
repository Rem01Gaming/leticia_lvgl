#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace Leticia {

enum class repeat_mode : uint8_t { off, track, list };

/**
 * Player options that survive a restart.
 */
struct playback_options_t {
    repeat_mode repeat = repeat_mode::track;
    bool shuffle = false;

    friend bool operator==(const playback_options_t &, const playback_options_t &) = default;
};

/**
 * Everything the player restores on the next start.
 */
struct playback_state_t {
    playback_options_t options;

    bool has_track = false;
    uint64_t track_hash = 0;
    uint64_t remaining_seconds = 0;

    /** Volume in percent, keyed by the hash of the output identity. */
    std::map<uint64_t, int> volumes;
};

namespace state_store {

inline constexpr int kDefaultVolumePercent = 50;

/**
 * Hashes a text with ChibiHash.
 *
 * @param text Input text.
 * @return 64 bit hash.
 */
uint64_t hash_text(const std::string &text);

/**
 * Hashes the path of a track relative to the music root.
 *
 * The relative form keeps the state valid when the card is mounted at another path.
 *
 * @param root Music scan path.
 * @param path Full track path.
 * @return 64 bit hash.
 */
uint64_t track_hash(const std::string &root, const std::string &path);

/**
 * Builds the path of the state file inside the music root.
 *
 * @param root Music scan path.
 * @return Path of the state file.
 */
std::string file_path(const std::string &root);

/**
 * Reads a state file. Unknown keys are skipped and numbers are range checked.
 *
 * @param path State file path.
 * @param out Receives the state. Left unchanged if the file is missing or has another version.
 * @return true if the file was read.
 */
bool load(const std::string &path, playback_state_t &out);

/**
 * Writes a state file through a temporary file and a rename, so a power cut never leaves half a file.
 *
 * @param path State file path.
 * @param state State to write.
 * @param durable true to flush the data to the storage before the rename.
 * @return true on success. On failure errno holds the cause.
 */
bool save(const std::string &path, const playback_state_t &state, bool durable);

} // namespace state_store

} // namespace Leticia
