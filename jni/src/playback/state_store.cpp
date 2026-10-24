#include "state_store.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

#include <chibihash64.h>

namespace Leticia::state_store {

namespace {

constexpr const char *kFileName = ".leticia_states";
constexpr const char *kTempSuffix = ".tmp";
constexpr const char *kVolumeKeyPrefix = "volume.";
constexpr int kFormatVersion = 1;
constexpr size_t kMaxFileBytes = 64 * 1024;
constexpr size_t kMaxVolumeEntries = 64;
constexpr uint64_t kHashSeed = 0;

/**
 * Parses an unsigned number. Signs, spaces and base prefixes are rejected.
 *
 * @param text Digits only.
 * @param base 10 or 16.
 * @param value Receives the number.
 * @return true if the whole text is a valid number.
 */
bool parse_unsigned(const std::string &text, int base, uint64_t &value) {
    if (text.empty()) return false;
    const char *begin = text.data();
    const char *end = begin + text.size();
    uint64_t parsed = 0;
    const auto result = std::from_chars(begin, end, parsed, base);
    if (result.ec != std::errc{} || result.ptr != end) return false;
    value = parsed;
    return true;
}

/**
 * Formats a hash as 16 lowercase hex digits.
 *
 * @param hash Hash value.
 * @return Hex text.
 */
std::string hex_text(uint64_t hash) {
    char text[17];
    snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

/**
 * Builds the file content.
 *
 * @param state State to write.
 * @return File content.
 */
std::string serialize(const playback_state_t &state) {
    std::string text;
    text += "version=" + std::to_string(kFormatVersion) + "\n";
    text += "repeat=" + std::to_string(static_cast<int>(state.options.repeat)) + "\n";
    text += std::string("shuffle=") + (state.options.shuffle ? "1" : "0") + "\n";
    if (state.has_track) {
        text += "track=" + hex_text(state.track_hash) + "\n";
        text += "remaining=" + std::to_string(state.remaining_seconds) + "\n";
    }
    for (const auto &[output_hash, percent] : state.volumes)
        text += kVolumeKeyPrefix + hex_text(output_hash) + "=" + std::to_string(percent) + "\n";
    return text;
}

/**
 * Applies one key and value pair to the state. Malformed pairs are skipped.
 *
 * @param key Key text.
 * @param value Value text.
 * @param state Receives the value.
 * @param version Receives the file version when the key is the version.
 */
void apply_pair(const std::string &key, const std::string &value, playback_state_t &state, uint64_t &version) {
    uint64_t number = 0;

    if (key == "version") {
        if (parse_unsigned(value, 10, number)) version = number;
    } else if (key == "repeat") {
        if (parse_unsigned(value, 10, number) && number <= static_cast<uint64_t>(repeat_mode::list))
            state.options.repeat = static_cast<repeat_mode>(number);
    } else if (key == "shuffle") {
        if (parse_unsigned(value, 10, number) && number <= 1) state.options.shuffle = number == 1;
    } else if (key == "track") {
        if (parse_unsigned(value, 16, number)) {
            state.track_hash = number;
            state.has_track = true;
        }
    } else if (key == "remaining") {
        if (parse_unsigned(value, 10, number)) state.remaining_seconds = number;
    } else if (key.rfind(kVolumeKeyPrefix, 0) == 0) {
        uint64_t output_hash = 0;
        uint64_t percent = 0;
        const std::string hash_part = key.substr(std::char_traits<char>::length(kVolumeKeyPrefix));
        if (state.volumes.size() < kMaxVolumeEntries && parse_unsigned(hash_part, 16, output_hash) &&
            parse_unsigned(value, 10, percent))
            state.volumes[output_hash] = static_cast<int>(std::min<uint64_t>(percent, 100));
    }
}

/**
 * Writes every byte of a text to a descriptor.
 *
 * @param fd Open descriptor.
 * @param text Text to write.
 * @return true if all bytes were written.
 */
bool write_all(int fd, const std::string &text) {
    size_t done = 0;
    while (done < text.size()) {
        const ssize_t written = write(fd, text.data() + done, text.size() - done);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += static_cast<size_t>(written);
    }
    return true;
}

} // namespace

uint64_t hash_text(const std::string &text) {
    return chibihash64(text.data(), static_cast<ptrdiff_t>(text.size()), kHashSeed);
}

uint64_t track_hash(const std::string &root, const std::string &path) {
    size_t start = 0;
    const bool under_root = !root.empty() && path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
                            (root.back() == '/' || path[root.size()] == '/');
    if (under_root) {
        start = root.size();
        while (start < path.size() && path[start] == '/') ++start;
    }
    return chibihash64(path.data() + start, static_cast<ptrdiff_t>(path.size() - start), kHashSeed);
}

std::string file_path(const std::string &root) {
    if (root.empty() || root.back() == '/') return root + kFileName;
    return root + "/" + kFileName;
}

bool load(const std::string &path, playback_state_t &out) {
    FILE *file = fopen(path.c_str(), "rb");
    if (file == nullptr) return false;

    std::string text(kMaxFileBytes + 1, '\0');
    const size_t length = fread(text.data(), 1, text.size(), file);
    fclose(file);
    if (length > kMaxFileBytes) return false;
    text.resize(length);

    playback_state_t parsed;
    uint64_t version = 0;
    size_t position = 0;
    while (position < text.size()) {
        size_t end = text.find('\n', position);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(position, end - position);
        position = end + 1;

        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        apply_pair(line.substr(0, equals), line.substr(equals + 1), parsed, version);
    }

    if (version != kFormatVersion) return false;
    out = std::move(parsed);
    return true;
}

bool save(const std::string &path, const playback_state_t &state, bool durable) {
    const std::string temp_path = path + kTempSuffix;
    const int fd = open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;

    bool ok = write_all(fd, serialize(state)) && (!durable || fsync(fd) == 0);
    int error = ok ? 0 : errno;
    if (close(fd) != 0 && ok) {
        ok = false;
        error = errno;
    }
    if (ok && rename(temp_path.c_str(), path.c_str()) != 0) {
        ok = false;
        error = errno;
    }
    if (!ok) unlink(temp_path.c_str());

    errno = error;
    return ok;
}

} // namespace Leticia::state_store
