#include "audio_library.hpp"
#include "util/updater_proto.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libswscale/swscale.h>
}

namespace Leticia::audio_library {

namespace {

constexpr const char *kProbeSize = "1048576";
constexpr const char *kAnalyzeDuration = "1000000";

/**
 * Joins a directory and a file name with one slash.
 *
 * @param dir Directory path.
 * @param file File name.
 * @return Joined path.
 */
std::string join_path(const std::string &dir, const std::string &file) {
    if (dir.empty()) return file;
    if (dir.back() == '/') return dir + file;
    return dir + "/" + file;
}

/**
 * Checks the file extension against the supported audio types.
 *
 * @param path File path.
 * @return true if the extension is an audio type.
 */
bool is_audio_file(const std::string &path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;

    std::string extension = path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    static const char *extensions[] = {".3ga",  ".3gp",  ".aac",  ".ac3",  ".aif",  ".aifc", ".aiff", ".alac", ".amr",  ".ape",
                                       ".au",   ".caf",  ".dff",  ".dsd",  ".dsf",  ".dts",  ".dtshd", ".eac3", ".flac",
                                       ".m4a",  ".m4b",  ".mka",  ".mlp",  ".mp2",  ".mp3",  ".mpc",  ".mpga", ".oga",
                                       ".ogg",  ".oma",  ".opus", ".ra",   ".rf64", ".shn",  ".spx",  ".tak",  ".thd",
                                       ".tta",  ".voc",  ".w64",  ".wav",  ".wave", ".weba", ".webm", ".wma",  ".wv"};
    return std::any_of(std::begin(extensions), std::end(extensions),
                       [&](const char *candidate) { return extension == candidate; });
}

/**
 * Opens a media file with bounded probing.
 *
 * @param path File path.
 * @param format Receives the format context.
 * @return true on success.
 */
bool open_media(const std::string &path, AVFormatContext **format) {
    AVDictionary *options = nullptr;
    av_dict_set(&options, "probesize", kProbeSize, 0);
    av_dict_set(&options, "analyzeduration", kAnalyzeDuration, 0);
    const int status = avformat_open_input(format, path.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (status < 0) return false;
    if (avformat_find_stream_info(*format, nullptr) < 0) {
        avformat_close_input(format);
        return false;
    }
    return true;
}

/**
 * Scales a decoded frame to a centered square in RGB565 little endian.
 *
 * @param frame Decoded picture.
 * @param size Edge length of the square, in pixels.
 * @param track Receives the cover pixels.
 * @return true on success.
 */
bool frame_to_cover(const AVFrame *frame, unsigned int size, music_track_t &track) {
    if (frame->width <= 0 || frame->height <= 0) return false;

    const int shortest = std::min(frame->width, frame->height);
    const int scaled_w = std::max<int>(static_cast<int>(size), static_cast<int>(static_cast<int64_t>(frame->width) * size / shortest));
    const int scaled_h = std::max<int>(static_cast<int>(size), static_cast<int>(static_cast<int64_t>(frame->height) * size / shortest));

    SwsContext *scale = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), scaled_w,
                                       scaled_h, AV_PIX_FMT_RGB565LE, SWS_AREA, nullptr, nullptr, nullptr);
    if (scale == nullptr) return false;

    std::vector<uint8_t> scaled(static_cast<size_t>(scaled_w) * static_cast<size_t>(scaled_h) * 2);
    uint8_t *destination[] = {scaled.data(), nullptr, nullptr, nullptr};
    int strides[] = {scaled_w * 2, 0, 0, 0};
    const int rows = sws_scale(scale, frame->data, frame->linesize, 0, frame->height, destination, strides);
    sws_freeContext(scale);
    if (rows <= 0) return false;

    const size_t offset_x = static_cast<size_t>(scaled_w - static_cast<int>(size)) / 2;
    const size_t offset_y = static_cast<size_t>(scaled_h - static_cast<int>(size)) / 2;
    track.cover_pixels.resize(static_cast<size_t>(size) * size * 2);
    for (unsigned int y = 0; y < size; ++y) {
        const uint8_t *from = scaled.data() + ((offset_y + y) * static_cast<size_t>(scaled_w) + offset_x) * 2;
        std::memcpy(track.cover_pixels.data() + static_cast<size_t>(y) * size * 2, from, static_cast<size_t>(size) * 2);
    }
    track.cover_width = size;
    track.cover_height = size;
    return true;
}

/**
 * Decodes one picture packet into the track cover.
 *
 * @param parameters Codec parameters of the picture stream.
 * @param packet Picture packet.
 * @param size Edge length of the square cover, in pixels.
 * @param track Receives the cover pixels.
 * @return true on success.
 */
bool decode_cover_packet(AVCodecParameters *parameters, const AVPacket *packet, unsigned int size, music_track_t &track) {
    const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
    if (codec == nullptr) return false;

    AVCodecContext *context = avcodec_alloc_context3(codec);
    if (context == nullptr) return false;

    AVFrame *frame = av_frame_alloc();
    bool decoded = false;
    if (frame != nullptr && avcodec_parameters_to_context(context, parameters) >= 0 &&
        avcodec_open2(context, codec, nullptr) >= 0 && avcodec_send_packet(context, packet) >= 0) {
        avcodec_send_packet(context, nullptr);
        if (avcodec_receive_frame(context, frame) >= 0) decoded = frame_to_cover(frame, size, track);
    }
    av_frame_free(&frame);
    avcodec_free_context(&context);
    return decoded;
}

/**
 * Decodes an image file in the album folder into the track cover.
 *
 * @param path Image file path.
 * @param size Edge length of the square cover, in pixels.
 * @param track Receives the cover pixels.
 * @return true on success.
 */
bool decode_cover_file(const std::string &path, unsigned int size, music_track_t &track) {
    AVFormatContext *format = nullptr;
    if (!open_media(path, &format)) return false;

    bool decoded = false;
    AVPacket *packet = av_packet_alloc();
    for (unsigned int index = 0; index < format->nb_streams && !decoded && packet != nullptr; ++index) {
        AVStream *stream = format->streams[index];
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        while (av_read_frame(format, packet) >= 0) {
            const bool match = packet->stream_index == static_cast<int>(index);
            if (match) decoded = decode_cover_packet(stream->codecpar, packet, size, track);
            av_packet_unref(packet);
            if (match) break;
        }
    }
    av_packet_free(&packet);
    avformat_close_input(&format);
    return decoded;
}

/**
 * Looks for a cover image next to the audio file.
 *
 * @param audio_path Audio file path.
 * @param size Edge length of the square cover, in pixels.
 * @param track Receives the cover pixels.
 */
void find_folder_cover(const std::string &audio_path, unsigned int size, music_track_t &track) {
    const size_t slash = audio_path.find_last_of('/');
    const std::string directory = slash == std::string::npos ? "." : audio_path.substr(0, slash);
    static const char *const names[] = {"cover.jpg", "cover.jpeg", "cover.png", "folder.jpg",
                                        "folder.png", "front.jpg", "album.jpg"};
    for (const char *name : names) {
        const std::string candidate = join_path(directory, name);
        if (access(candidate.c_str(), R_OK) == 0 && decode_cover_file(candidate, size, track)) return;
    }
}

/**
 * Reads one metadata tag.
 *
 * @param metadata Tag dictionary.
 * @param key Tag name.
 * @return Tag value or an empty string.
 */
std::string metadata_value(AVDictionary *metadata, const char *key) {
    const AVDictionaryEntry *entry = av_dict_get(metadata, key, nullptr, AV_DICT_IGNORE_SUFFIX);
    return entry != nullptr && entry->value != nullptr ? entry->value : "";
}

constexpr int kMaxScanDepth = 12;

/**
 * Collects audio files below a directory with a depth limit against symlink loops.
 *
 * @param directory Directory to scan.
 * @param files Receives the file paths.
 * @param depth Current recursion depth.
 */
void collect_recursive(const std::string &directory, std::vector<std::string> &files, int depth) {
    DIR *dir = opendir(directory.c_str());
    if (dir == nullptr) {
        Leticia::ui_print("audio_library: cannot scan %s: %s", directory.c_str(), strerror(errno));
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;

        std::string path = join_path(directory, entry->d_name);
        struct stat info {};
        if (stat(path.c_str(), &info) != 0) continue;
        if (S_ISDIR(info.st_mode)) {
            if (depth < kMaxScanDepth) collect_recursive(path, files, depth + 1);
        } else if (S_ISREG(info.st_mode) && is_audio_file(path)) {
            files.push_back(std::move(path));
        }
    }
    closedir(dir);
}

} // namespace

void collect_audio_files(const std::string &directory, std::vector<std::string> &files) {
    collect_recursive(directory, files, 0);
}

music_track_t read_track(const std::string &path, unsigned int cover_size_px) {
    music_track_t track;
    track.path = path;
    const size_t slash = path.find_last_of('/');
    const size_t dot = path.find_last_of('.');
    const size_t name_start = slash == std::string::npos ? 0 : slash + 1;
    const size_t name_end = dot != std::string::npos && dot > name_start ? dot : path.size();
    track.title = path.substr(name_start, name_end - name_start);
    track.artist = "Unknown artist";

    AVFormatContext *format = nullptr;
    if (!open_media(path, &format)) {
        track.path.clear();
        return track;
    }

    bool has_audio = false;
    for (unsigned int index = 0; index < format->nb_streams; ++index) {
        AVStream *stream = format->streams[index];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && avcodec_find_decoder(stream->codecpar->codec_id) != nullptr)
            has_audio = true;
        if ((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) && track.cover_pixels.empty())
            decode_cover_packet(stream->codecpar, &stream->attached_pic, cover_size_px, track);
    }

    if (!has_audio) {
        avformat_close_input(&format);
        track.path.clear();
        return track;
    }

    const std::string title = metadata_value(format->metadata, "title");
    const std::string artist = metadata_value(format->metadata, "artist");
    if (!title.empty()) track.title = title;
    if (!artist.empty()) track.artist = artist;
    if (format->duration > 0) track.duration_seconds = static_cast<uint64_t>(format->duration / AV_TIME_BASE);
    avformat_close_input(&format);

    if (track.cover_pixels.empty()) find_folder_cover(path, cover_size_px, track);
    return track;
}

} // namespace Leticia::audio_library
