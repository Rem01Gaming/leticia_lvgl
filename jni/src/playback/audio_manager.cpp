#include "audio_manager.hpp"
#include "alsa_scan.hpp"
#include "audio_library.hpp"
#include "resampler.hpp"
#include "util/updater_proto.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <poll.h>
#include <sched.h>
#include <sys/syscall.h>
#include <tuple>
#include <unistd.h>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

namespace Leticia {

namespace {

constexpr uint32_t kDevicePollIntervalMs = 250;
constexpr uint32_t kInputPollIntervalMs = 40;
constexpr int kVolumeStepPercent = 5;
constexpr uint32_t kVolumeRepeatDelayMs = 400;
constexpr uint32_t kVolumeRepeatIntervalMs = 80;
constexpr int kDeviceOpenRetries = 8;
constexpr auto kDeviceOpenRetryDelay = std::chrono::milliseconds(60);
constexpr int kMaxConsecutiveWriteErrors = 50;
constexpr int kWritePollTimeoutMs = 20;
constexpr uint32_t kStateSaveDelayMs = 1500;
constexpr uint32_t kPositionSaveIntervalMs = 10000;

/**
 * Maps an onboard output to its UCM device name.
 *
 * @param out Onboard output.
 * @return UCM device name or nullptr.
 */
const char *device_name_for(audio_output out) {
    switch (out) {
        case audio_output::speaker: return "Speaker";
        case audio_output::headphones: return "Headphones";
        case audio_output::unknown: return nullptr;
    }
    return nullptr;
}

/**
 * Pins the calling thread to the given CPUs.
 *
 * @param cpus CPU indexes. An empty list keeps the default affinity.
 */
void apply_audio_cpu_affinity(const std::vector<int> &cpus) {
    if (cpus.empty()) return;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int cpu_id : cpus) {
        if (cpu_id < 0 || cpu_id >= CPU_SETSIZE) return;
        CPU_SET(cpu_id, &cpuset);
    }
    if (sched_setaffinity(static_cast<pid_t>(syscall(SYS_gettid)), sizeof(cpuset), &cpuset) != 0)
        Leticia::ui_print("audio_manager: sched_setaffinity failed: %s", strerror(errno));
}

/**
 * Pairs an ALSA sample format with the FFmpeg format that feeds it.
 */
struct pcm_format_mapping {
    ModernAlsa::sample_format alsa_format;
    AVSampleFormat av_format;
    size_t bytes_per_sample;
    unsigned int valid_bits;
    unsigned int post_shift;
    bool packed_24;
};

constexpr pcm_format_mapping kSupportedFormats[] = {
    {ModernAlsa::sample_format::s32_le, AV_SAMPLE_FMT_S32, sizeof(int32_t), 32, 0, false},
    {ModernAlsa::sample_format::s24_le, AV_SAMPLE_FMT_S32, sizeof(int32_t), 24, 8, false},
    {ModernAlsa::sample_format::s24_3le, AV_SAMPLE_FMT_S32, 3, 24, 0, true},
    {ModernAlsa::sample_format::s16_le, AV_SAMPLE_FMT_S16, sizeof(int16_t), 16, 0, false},
};

/**
 * Orders the sample rates to try. The source rate comes first.
 * Rates of the same family (44.1 kHz or 48 kHz) and rates above the source come next.
 *
 * @param source_rate Rate of the decoded stream.
 * @return Rates in order of preference.
 */
std::vector<unsigned int> candidate_rates(unsigned int source_rate) {
    std::vector<unsigned int> rates = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100,
                                       48000, 88200, 96000, 176400, 192000};
    if (std::find(rates.begin(), rates.end(), source_rate) == rates.end() && source_rate != 0)
        rates.push_back(source_rate);

    const bool source_family = source_rate % 11025 == 0;
    std::stable_sort(rates.begin(), rates.end(), [&](unsigned int a, unsigned int b) {
        auto key = [&](unsigned int r) {
            const long distance = r > source_rate ? static_cast<long>(r - source_rate) : static_cast<long>(source_rate - r);
            return std::make_tuple(r != source_rate, (r % 11025 == 0) != source_family, r < source_rate, distance);
        };
        return key(a) < key(b);
    });
    return rates;
}

/**
 * Converts a volume percentage to a linear amplitude gain.
 *
 * The curve is quadratic because perceived loudness is closer to logarithmic than linear.
 * 100 percent gives exactly 1.0, so full volume leaves the samples untouched.
 *
 * @param percent Volume in percent.
 * @return Gain from 0.0 to 1.0.
 */
double volume_gain(int percent) {
    const double normalized = static_cast<double>(std::clamp(percent, 0, 100)) / 100.0;
    return normalized * normalized;
}

/**
 * Scales interleaved samples in place and ramps the gain across the block.
 *
 * The ramp avoids audible clicks when the volume changes between two blocks.
 *
 * @tparam Sample Sample type, int16_t or int32_t.
 * @param samples Interleaved samples.
 * @param frames Number of frames in the block.
 * @param channels Number of channels.
 * @param from_gain Gain at the start of the block.
 * @param to_gain Gain at the end of the block.
 */
template <typename Sample>
void scale_samples(Sample *samples, size_t frames, size_t channels, double from_gain, double to_gain) {
    const double step = frames != 0 ? (to_gain - from_gain) / static_cast<double>(frames) : 0.0;
    double gain = from_gain;
    for (size_t frame = 0; frame < frames; ++frame) {
        gain += step;
        for (size_t channel = 0; channel < channels; ++channel) {
            Sample &sample = samples[frame * channels + channel];
            sample = static_cast<Sample>(std::llrint(static_cast<double>(sample) * gain));
        }
    }
}

/**
 * Chooses the best sample format, channel count and rate that the device accepts.
 *
 * @param params Capabilities of the device.
 * @param supported Sample formats of the device.
 * @param max_bit_depth Bit depth limit from the device config. 0 means no limit.
 * @param source_rate Rate of the decoded stream.
 * @param requested_channels Preferred channel count.
 * @param format Receives the chosen format.
 * @param rate Receives the chosen rate.
 * @param channels Receives the chosen channel count.
 * @return true if a combination works, false otherwise.
 */
bool choose_output_config(const ModernAlsa::pcm_params &params, const std::vector<ModernAlsa::sample_format> &supported,
                          unsigned int max_bit_depth, unsigned int source_rate, size_t requested_channels,
                          const pcm_format_mapping **format, unsigned int *rate, size_t *channels) {
    std::vector<size_t> channel_options{requested_channels};
    for (size_t candidate : {size_t{2}, size_t{1}, size_t{4}, size_t{6}, size_t{8}})
        if (std::find(channel_options.begin(), channel_options.end(), candidate) == channel_options.end())
            channel_options.push_back(candidate);

    const std::vector<unsigned int> rates = candidate_rates(source_rate);
    for (const auto &candidate : kSupportedFormats) {
        if (max_bit_depth != 0 && candidate.valid_bits > max_bit_depth) continue;
        if (std::find(supported.begin(), supported.end(), candidate.alsa_format) == supported.end()) continue;
        for (size_t candidate_channels : channel_options) {
            if (candidate_channels == 0 || !params.test_channels(candidate_channels)) continue;
            for (unsigned int candidate_rate : rates) {
                if (!params.test_config(candidate_channels, candidate_rate, candidate.alsa_format)) continue;
                *format = &candidate;
                *rate = candidate_rate;
                *channels = candidate_channels;
                return true;
            }
        }
    }
    return false;
}

/**
 * Fills the capability fields of a device description.
 *
 * @param params Capabilities of the device.
 * @param supported Sample formats of the device.
 * @param info Receives the capability strings.
 */
void describe_capabilities(const ModernAlsa::pcm_params &params, const std::vector<ModernAlsa::sample_format> &supported,
                           audio_device_info_t &info) {
    std::string formats;
    for (ModernAlsa::sample_format format : supported) {
        for (const auto &mapping : kSupportedFormats) {
            if (mapping.alsa_format != format) continue;
            if (!formats.empty()) formats += ", ";
            formats += ModernAlsa::to_string(format);
        }
    }
    info.supported_formats = formats.empty() ? "-" : formats;

    auto min_rate = params.get_min_rate();
    auto max_rate = params.get_max_rate();
    if (!min_rate.failed() && !max_rate.failed()) {
        char text[64];
        snprintf(text, sizeof(text), "%lu-%lu Hz", static_cast<unsigned long>(min_rate.unwrap()),
                 static_cast<unsigned long>(max_rate.unwrap()));
        info.supported_rates = text;
    }

    auto max_channels = params.get_max_channels();
    if (!max_channels.failed()) info.max_channels = static_cast<unsigned int>(max_channels.unwrap());
}

/**
 * Collects the sample formats of a device.
 *
 * @param params Capabilities of the device.
 * @return Supported sample formats.
 */
std::vector<ModernAlsa::sample_format> list_formats(const ModernAlsa::pcm_params &params) {
    std::vector<ModernAlsa::sample_format> formats;
    params.for_each_supported_format(
        [](ModernAlsa::sample_format fmt, void *user) {
            static_cast<std::vector<ModernAlsa::sample_format> *>(user)->push_back(fmt);
        },
        &formats);
    return formats;
}

/**
 * Retries a device open. A USB DAC refuses a new open for a short time after a close.
 *
 * @param fn Function that opens the device.
 * @return Result of the last attempt.
 */
template <typename OpenFn>
ModernAlsa::result open_device_with_retry(OpenFn &&fn) {
    ModernAlsa::result r;
    for (int attempt = 0; attempt < kDeviceOpenRetries; attempt++) {
        r = fn();
        if (!r.failed()) return r;
        if (attempt + 1 < kDeviceOpenRetries) std::this_thread::sleep_for(kDeviceOpenRetryDelay);
    }
    return r;
}

/**
 * Owns the FFmpeg objects of one playback session.
 */
struct av_session {
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    int stream_index = -1;

    av_session() = default;
    av_session(const av_session &) = delete;
    av_session &operator=(const av_session &) = delete;

    ~av_session() {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
    }

    /**
     * Opens the file and its audio decoder.
     *
     * @param path File path.
     * @return true on success.
     */
    bool open(const std::string &path) {
        if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) return false;
        if (avformat_find_stream_info(format, nullptr) < 0) return false;

        for (unsigned int i = 0; i < format->nb_streams; i++) {
            if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                stream_index = static_cast<int>(i);
                break;
            }
        }
        if (stream_index < 0) return false;

        AVStream *stream = format->streams[stream_index];
        const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (decoder == nullptr) return false;

        codec = avcodec_alloc_context3(decoder);
        if (codec == nullptr || avcodec_parameters_to_context(codec, stream->codecpar) < 0) return false;
        if (avcodec_open2(codec, decoder, nullptr) < 0) return false;

        packet = av_packet_alloc();
        frame = av_frame_alloc();
        return packet != nullptr && frame != nullptr;
    }

    AVStream *stream() const { return format->streams[stream_index]; }
};

enum class write_status {
    ok,
    stopped,
    disconnected,
};

/**
 * Checks whether the kernel reports the device as unplugged.
 *
 * @param writer Open PCM writer.
 * @param error Error code of the failed call.
 * @return true if the device is gone.
 */
bool device_is_gone(const ModernAlsa::interleaved_pcm_writer &writer, int error) {
    if (error == ENODEV || error == ENOTTY || error == EBADFD) return true;
    auto state = writer.get_state();
    return !state.failed() && state.value == ModernAlsa::pcm_state::disconnected;
}

/**
 * Writes all frames to the device. Waits on the device descriptor when the buffer is full.
 *
 * @param writer Open PCM writer.
 * @param data Interleaved frames.
 * @param frames Number of frames.
 * @param frame_bytes Bytes per frame.
 * @param stop Flag that cancels the write.
 * @return Result of the write.
 */
write_status write_frames(ModernAlsa::interleaved_pcm_writer &writer, const uint8_t *data, ModernAlsa::size_type frames,
                          size_t frame_bytes, const std::atomic<bool> &stop) {
    ModernAlsa::size_type written = 0;
    int errors = 0;

    while (written < frames) {
        if (stop.load(std::memory_order_acquire)) return write_status::stopped;
        if (errors >= kMaxConsecutiveWriteErrors) return write_status::disconnected;

        auto avail = writer.get_avail();
        if (avail.failed()) {
            if (device_is_gone(writer, avail.error)) return write_status::disconnected;
            ModernAlsa::pcm_recover(writer, avail.error, true);
            errors++;
            continue;
        }

        const ModernAlsa::size_type room = avail.value;
        if (room == 0) {
            auto state = writer.get_state();
            if (!state.failed() && state.value == ModernAlsa::pcm_state::disconnected) return write_status::disconnected;
            if (!state.failed() && state.value == ModernAlsa::pcm_state::xrun) writer.prepare();

            struct pollfd pfd = {writer.get_file_descriptor(), static_cast<short>(writer.get_poll_events()), 0};
            poll(&pfd, 1, kWritePollTimeoutMs);
            continue;
        }

        const ModernAlsa::size_type chunk = std::min(room, frames - written);
        auto result = writer.write_unformatted(data + written * frame_bytes, chunk);
        if (result.failed()) {
            if (device_is_gone(writer, result.error)) return write_status::disconnected;
            ModernAlsa::pcm_recover(writer, result.error, true);
            errors++;
            continue;
        }
        errors = 0;
        written += result.value;
    }
    return write_status::ok;
}

/**
 * Packs the top three bytes of each 32 bit sample into a 24 bit little endian stream.
 *
 * @param samples Input samples.
 * @param count Number of samples.
 * @param out Output bytes. Must hold 3 * count bytes.
 */
void pack_s24_3le(const int32_t *samples, size_t count, uint8_t *out) {
    for (size_t i = 0; i < count; i++) {
        const int32_t v = samples[i];
        out[3 * i] = static_cast<uint8_t>(v >> 8);
        out[3 * i + 1] = static_cast<uint8_t>(v >> 16);
        out[3 * i + 2] = static_cast<uint8_t>(v >> 24);
    }
}

} // namespace

audio_manager::~audio_manager() {
    deinit();
}

audio_output audio_manager::detected_output() const {
    if (input_monitor_.headphone_state_known() && input_monitor_.is_headphone_connected())
        return audio_output::headphones;
    return audio_output::speaker;
}

bool audio_manager::init(const device_config_t &device_config, const user_config_t &user_config,
                         const std::string &zip_path, unsigned int cover_size_px) {
    card_ = device_config.alsa_card;
    cpu_affinity_ = device_config.audio_thread_affinity;
    onboard_reopen_workaround_ = device_config.onboard_audio_reopen_workaround;
    max_bit_depth_ = device_config.audio_max_bit_depth;
    cover_size_px_ = cover_size_px;
    scan_path_ = user_config.music_scan_path;

    if (!scan_path_.empty()) {
        state_path_ = state_store::file_path(scan_path_);
        if (state_store::load(state_path_, state_))
            Leticia::ui_print("audio_manager: loaded playback state from %s", state_path_.c_str());
    }

    std::string ucm_path;
    if (resolve_alsa_ucm_config_path(zip_path, ucm_path)) {
        ucm::parse_error err = cfg_.parse_file(ucm_path.c_str());
        if (err.failed()) {
            Leticia::ui_print("audio_manager: UCM config parse error: %s (line %u)", err.description(), err.line);
        } else {
            cfg_loaded_ = true;
            Leticia::ui_print("audio_manager: loaded UCM config for card '%s'", cfg_.card_name);
        }
    }

    if (cfg_loaded_) {
        ModernAlsa::result r = mixer_.open(card_);
        if (r.failed()) {
            Leticia::ui_print("audio_manager: cannot open mixer of card %u: %s", card_, r.error_description());
            cfg_loaded_ = false;
        } else {
            auto info = mixer_.get_card_info();
            if (!info.failed() && strstr(info.value.name, cfg_.card_name) == nullptr)
                Leticia::ui_print("audio_manager: mixer reports '%s', config expects '%s'", info.value.name, cfg_.card_name);
        }
    }

    if (input_monitor_.open()) {
        input_monitor_.set_callback([this](const input_event_t &event) { on_input_event(event); });
    } else {
        Leticia::ui_print("audio_manager: no jack input source, assuming speaker");
    }

    detected_output_ = detected_output();
    volume_output_hash_ = current_output_hash();
    volume_percent_.store(saved_volume(volume_output_hash_), std::memory_order_release);

    unsigned int usb_card = 0;
    unsigned int usb_device = 0;
    usb_present_ = alsa_scan::find_usb_playback(card_, usb_card, usb_device);
    pcm_signature_ = alsa_scan::playback_signature();
    refresh_idle_device_info();

    last_device_check_ms_ = lv_tick_get();
    poll_timer_ = lv_timer_create(poll_timer_trampoline, kInputPollIntervalMs, this);

    scan_thread_ = std::thread(&audio_manager::scan_worker, this);
    return is_available();
}

void audio_manager::scan_worker() {
    std::vector<std::string> files;
    if (!scan_path_.empty()) audio_library::collect_audio_files(scan_path_, files);
    std::sort(files.begin(), files.end());

    scanned_.reserve(files.size());
    for (const std::string &path : files) {
        if (scan_abort_.load(std::memory_order_acquire)) break;
        music_track_t track = audio_library::read_track(path, cover_size_px_);
        if (!track.path.empty()) scanned_.push_back(std::move(track));
        scan_progress_.fetch_add(1, std::memory_order_relaxed);
    }
    Leticia::ui_print("audio_manager: indexed %zu music files under %s", scanned_.size(), scan_path_.c_str());
    scan_done_.store(true, std::memory_order_release);
}

bool audio_manager::take_scan_result() {
    if (!scan_done_.exchange(false, std::memory_order_acq_rel)) return false;

    if (scan_thread_.joinable()) scan_thread_.join();
    tracks_ = std::move(scanned_);
    scanned_.clear();
    current_track_index_ = 0;
    if (!tracks_.empty()) music_file_path_ = tracks_.front().path;
    restore_last_track();
    state_ready_ = true;
    return true;
}

void audio_manager::restore_last_track() {
    if (!state_.has_track) return;

    for (size_t index = 0; index < tracks_.size(); ++index) {
        if (state_store::track_hash(scan_path_, tracks_[index].path) != state_.track_hash) continue;

        current_track_index_ = index;
        music_file_path_ = tracks_[index].path;
        track_state_valid_ = true;

        const uint64_t duration = tracks_[index].duration_seconds;
        const uint64_t remaining = state_.remaining_seconds;
        if (duration != 0 && remaining != 0 && remaining < duration) {
            position_seconds_.store(duration - remaining, std::memory_order_release);
            resume_pending_.store(true, std::memory_order_release);
        }
        return;
    }
}

uint64_t audio_manager::current_output_hash() const {
    unsigned int usb_card = 0;
    unsigned int usb_device = 0;
    if (alsa_scan::find_usb_playback(card_, usb_card, usb_device))
        return state_store::hash_text("usb:" + alsa_scan::card_label(usb_card));
    return state_store::hash_text(detected_output() == audio_output::headphones ? "headphones" : "speaker");
}

int audio_manager::saved_volume(uint64_t output_hash) const {
    const auto found = state_.volumes.find(output_hash);
    if (found == state_.volumes.end()) return state_store::kDefaultVolumePercent;
    return std::clamp(found->second, 0, 100);
}

void audio_manager::sync_volume_to_output(bool announce) {
    const uint64_t output_hash = current_output_hash();
    if (output_hash == volume_output_hash_) return;

    volume_output_hash_ = output_hash;
    volume_percent_.store(saved_volume(output_hash), std::memory_order_release);
    if (announce) volume_changed_.store(true, std::memory_order_release);
}

void audio_manager::set_options(const playback_options_t &options) {
    if (options == state_.options) return;
    state_.options = options;
    mark_state_dirty();
}

void audio_manager::mark_state_dirty() {
    if (state_dirty_) return;
    state_dirty_ = true;
    state_save_at_ms_ = lv_tick_get() + kStateSaveDelayMs;
}

void audio_manager::service_state_save(uint32_t now_ms) {
    if (!state_ready_) return;
    if (is_audible() && now_ms - last_state_save_ms_ >= kPositionSaveIntervalMs) mark_state_dirty();
    if (state_dirty_ && static_cast<int32_t>(now_ms - state_save_at_ms_) >= 0) save_state(false);
}

void audio_manager::save_state(bool durable) {
    state_dirty_ = false;
    last_state_save_ms_ = lv_tick_get();
    if (state_path_.empty()) return;

    const music_track_t *track = current_track();
    if (track_state_valid_ && track != nullptr) {
        const uint64_t duration = track->duration_seconds;
        const uint64_t position = std::min(position_seconds_.load(std::memory_order_acquire), duration);
        state_.has_track = true;
        state_.track_hash = state_store::track_hash(scan_path_, track->path);
        state_.remaining_seconds = duration - position;
    }

    if (state_store::save(state_path_, state_, durable)) {
        state_write_failed_ = false;
    } else if (!state_write_failed_) {
        state_write_failed_ = true;
        Leticia::ui_print("audio_manager: cannot save playback state to %s: %s", state_path_.c_str(), strerror(errno));
    }
}

const music_track_t *audio_manager::current_track() const {
    return current_track_index_ < tracks_.size() ? &tracks_[current_track_index_] : nullptr;
}

void audio_manager::deinit() {
    scan_abort_.store(true, std::memory_order_release);
    if (scan_thread_.joinable()) scan_thread_.join();

    if (state_ready_) save_state(true);
    state_ready_ = false;

    stop_playback();

    if (poll_timer_ != nullptr) {
        lv_timer_delete(poll_timer_);
        poll_timer_ = nullptr;
    }

    input_monitor_.close();
    mixer_.close();
    cfg_loaded_ = false;
}

bool audio_manager::apply_output(audio_output target) {
    if (!cfg_loaded_ || !mixer_.is_open()) return false;

    const char *new_name = device_name_for(target);
    if (new_name == nullptr) return false;

    ucm::result r = ucm::apply(mixer_, cfg_, "HiFi", new_name, device_name_for(mixer_applied_output_));
    if (r.failed()) {
        Leticia::ui_print("audio_manager: ucm apply %s failed: %s", new_name, r.error_description());
        return false;
    }

    Leticia::ui_print("audio_manager: routed to %s", new_name);
    mixer_applied_output_ = target;
    return true;
}

void audio_manager::mute_output() {
    if (!cfg_loaded_ || !mixer_.is_open()) return;

    const char *applied_name = device_name_for(mixer_applied_output_);
    if (applied_name == nullptr) return;

    ucm::result r = ucm::mute(mixer_, cfg_, "HiFi", applied_name);
    if (r.failed()) Leticia::ui_print("audio_manager: ucm mute %s failed: %s", applied_name, r.error_description());
    mixer_applied_output_ = audio_output::unknown;
}

void audio_manager::set_volume(int percent) {
    const int clamped = std::clamp(percent, 0, 100);
    volume_percent_.store(clamped, std::memory_order_release);
    state_.volumes[volume_output_hash_] = clamped;
    volume_changed_.store(true, std::memory_order_release);
    mark_state_dirty();
}

void audio_manager::adjust_volume(int delta_percent) {
    set_volume(volume() + delta_percent);
}

void audio_manager::on_volume_key(bool up, bool pressed) {
    (up ? volume_up_held_ : volume_down_held_) = pressed;

    if (pressed) {
        volume_repeat_direction_ = up ? 1 : -1;
        volume_next_repeat_ms_ = lv_tick_get() + kVolumeRepeatDelayMs;
        adjust_volume(volume_repeat_direction_ * kVolumeStepPercent);
        return;
    }

    /* Keep repeating for the other key if it is still held, otherwise stop. */
    if (volume_up_held_)
        volume_repeat_direction_ = 1;
    else if (volume_down_held_)
        volume_repeat_direction_ = -1;
    else
        volume_repeat_direction_ = 0;
    volume_next_repeat_ms_ = lv_tick_get() + kVolumeRepeatDelayMs;
}

void audio_manager::service_volume_repeat(uint32_t now_ms) {
    if (volume_repeat_direction_ == 0) return;
    if (static_cast<int32_t>(now_ms - volume_next_repeat_ms_) < 0) return;

    adjust_volume(volume_repeat_direction_ * kVolumeStepPercent);
    volume_next_repeat_ms_ = now_ms + kVolumeRepeatIntervalMs;
}

void audio_manager::on_input_event(const input_event_t &event) {
    switch (event.type) {
    case input_event_type::volume_up_press:
        on_volume_key(true, true);
        return;
    case input_event_type::volume_up_release:
        on_volume_key(true, false);
        return;
    case input_event_type::volume_down_press:
        on_volume_key(false, true);
        return;
    case input_event_type::volume_down_release:
        on_volume_key(false, false);
        return;
    default:
        break;
    }
    if (event.type != input_event_type::headphone_insert && event.type != input_event_type::headphone_remove) return;

    detected_output_ = detected_output();

    const bool onboard_session = playing_.load(std::memory_order_acquire) && !via_usb_active_.load(std::memory_order_acquire);
    if (onboard_session) {
        if (event.type == input_event_type::headphone_remove)
            pause_for_route_loss();
        else
            restart_playback();
    }
    sync_volume_to_output(true);
    refresh_idle_device_info();
}

void audio_manager::on_usb_changed(bool present) {
    const bool playing = playing_.load(std::memory_order_acquire);
    const bool via_usb = via_usb_active_.load(std::memory_order_acquire);

    if (present && playing && !via_usb) {
        Leticia::ui_print("audio_manager: USB DAC connected, switching to it");
        restart_playback();
    } else if (!present && playing && via_usb) {
        Leticia::ui_print("audio_manager: USB DAC removed, pausing");
        pause_for_route_loss();
    }
    sync_volume_to_output(true);
    refresh_idle_device_info();
}

void audio_manager::pause_for_route_loss() {
    const uint64_t resume_at = position_seconds_.load(std::memory_order_acquire);
    stop_playback();
    sync_volume_to_output(true);
    position_seconds_.store(resume_at, std::memory_order_release);
    resume_pending_.store(true, std::memory_order_release);
    auto_paused_.store(true, std::memory_order_release);
}

void audio_manager::restart_playback() {
    const bool was_paused = paused_.load(std::memory_order_acquire);
    pause_for_route_loss();
    if (!was_paused) start_playback();
}

bool audio_manager::start_playback() {
    if (tracks_.empty() || current_track_index_ >= tracks_.size()) {
        Leticia::ui_print("audio_manager: no music to play");
        return false;
    }

    bool expected = false;
    if (!playing_.compare_exchange_strong(expected, true)) {
        paused_.store(false, std::memory_order_release);
        auto_paused_.store(false, std::memory_order_release);
        return true;
    }

    if (playback_thread_.joinable()) playback_thread_.join();

    const bool resume = resume_pending_.exchange(false, std::memory_order_acq_rel);
    const uint64_t resume_at = position_seconds_.load(std::memory_order_acquire);
    should_stop_.store(false, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    auto_paused_.store(false, std::memory_order_release);
    track_finished_.store(false, std::memory_order_release);
    if (!resume) position_seconds_.store(0, std::memory_order_release);
    seek_request_seconds_.store(resume && resume_at > 0 ? static_cast<int64_t>(resume_at) : -1, std::memory_order_release);
    music_file_path_ = tracks_[current_track_index_].path;
    track_state_valid_ = true;
    mark_state_dirty();
    playback_thread_ = std::thread(&audio_manager::playback_thread_func, this);
    return true;
}

void audio_manager::stop_playback() {
    should_stop_.store(true, std::memory_order_release);
    if (playback_thread_.joinable()) playback_thread_.join();
    playing_.store(false, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    auto_paused_.store(false, std::memory_order_release);
}

bool audio_manager::toggle_playback() {
    if (playing_.load(std::memory_order_acquire)) {
        const bool pause = !paused_.load(std::memory_order_acquire);
        paused_.store(pause, std::memory_order_release);
        mark_state_dirty();
        return !pause;
    }
    return start_playback();
}

bool audio_manager::play_track(size_t index) {
    if (index >= tracks_.size()) return false;
    stop_playback();
    current_track_index_ = index;
    position_seconds_.store(0, std::memory_order_release);
    resume_pending_.store(false, std::memory_order_release);
    return start_playback();
}

void audio_manager::seek_to(uint64_t seconds) {
    const music_track_t *track = current_track();
    if (track == nullptr) return;
    if (track->duration_seconds != 0 && seconds > track->duration_seconds) seconds = track->duration_seconds;

    position_seconds_.store(seconds, std::memory_order_release);
    if (!playing_.load(std::memory_order_acquire)) resume_pending_.store(true, std::memory_order_release);
    seek_request_seconds_.store(static_cast<int64_t>(seconds), std::memory_order_release);
    mark_state_dirty();
}

audio_device_info_t audio_manager::device_info() const {
    std::lock_guard<std::mutex> lock(device_info_mutex_);
    audio_device_info_t info = device_info_;
    info.active = playing_.load(std::memory_order_acquire);
    return info;
}

void audio_manager::refresh_idle_device_info() {
    if (playing_.load(std::memory_order_acquire)) return;

    audio_device_info_t info;
    unsigned int card = card_;
    unsigned int device = 0;
    unsigned int usb_card = 0;
    unsigned int usb_device = 0;

    if (alsa_scan::find_usb_playback(card_, usb_card, usb_device)) {
        card = usb_card;
        device = usb_device;
        info.output = "USB DAC";
        info.name = alsa_scan::card_label(usb_card);
    } else {
        info.output = detected_output() == audio_output::headphones ? "Headphones" : "Speaker";
        info.name = "Onboard audio";
        if (mixer_.is_open()) {
            auto card_info = mixer_.get_card_info();
            if (!card_info.failed()) info.name = card_info.value.name;
        }
        const ucm::verb *verb = cfg_loaded_ ? cfg_.find_verb("HiFi") : nullptr;
        if (verb != nullptr && verb->playback_pcm_device != ucm::invalid_index()) device = verb->playback_pcm_device;
    }
    info.card = card;
    info.device = device;

    ModernAlsa::pcm_params params;
    if (!params.open(card, device, false).failed()) describe_capabilities(params, list_formats(params), info);

    std::lock_guard<std::mutex> lock(device_info_mutex_);
    device_info_ = info;
}

void audio_manager::poll_timer_trampoline(lv_timer_t *timer) {
    auto *self = static_cast<audio_manager *>(lv_timer_get_user_data(timer));
    self->input_monitor_.poll();

    /* Keys are read often so the volume reacts at once. The device scan stays on its slower period. */
    const uint32_t now = lv_tick_get();
    self->service_volume_repeat(now);
    self->service_state_save(now);
    if (now - self->last_device_check_ms_ < kDevicePollIntervalMs) return;
    self->last_device_check_ms_ = now;

    const uint64_t signature = alsa_scan::playback_signature();
    if (signature == self->pcm_signature_) return;
    self->pcm_signature_ = signature;

    unsigned int usb_card = 0;
    unsigned int usb_device = 0;
    const bool present = alsa_scan::find_usb_playback(self->card_, usb_card, usb_device);
    if (present != self->usb_present_) {
        self->usb_present_ = present;
        self->on_usb_changed(present);
    } else {
        self->sync_volume_to_output(true);
        self->refresh_idle_device_info();
    }
}

void audio_manager::playback_thread_func() {
    apply_audio_cpu_affinity(cpu_affinity_);

    auto finish = [this](const char *reason) {
        if (reason != nullptr) Leticia::ui_print("audio_manager: playback aborted: %s", reason);
        playing_.store(false, std::memory_order_release);
        paused_.store(false, std::memory_order_release);
    };

    output_target target;
    audio_output onboard_output = audio_output::unknown;
    bool route_onboard = false;

    unsigned int usb_card = 0;
    unsigned int usb_device = 0;
    if (alsa_scan::find_usb_playback(card_, usb_card, usb_device)) {
        target.card = usb_card;
        target.device = usb_device;
        target.via_usb = true;
    } else if (cfg_loaded_ && mixer_.is_open()) {
        onboard_output = detected_output();
        route_onboard = true;
        target.card = card_;
        target.device = 0;
        const ucm::verb *verb = cfg_.find_verb("HiFi");
        if (verb != nullptr && verb->playback_pcm_device != ucm::invalid_index()) target.device = verb->playback_pcm_device;
        const ucm::device *dev = verb != nullptr ? verb->find_device(device_name_for(onboard_output)) : nullptr;
        if (dev != nullptr && dev->channels != 0) target.channels = dev->channels;
    } else {
        finish("no USB DAC and no UCM config");
        return;
    }
    via_usb_active_.store(target.via_usb, std::memory_order_release);

    av_session av;
    if (!av.open(music_file_path_)) {
        finish("cannot open the file");
        return;
    }
    AVStream *stream = av.stream();

    // The verb-level controls (e.g. "ADDA_DL_CH1 DL2_CH1") are what connect the PCM front-end to the
    // codec back-end. On a DPCM card the PCM refuses to prepare, with EINVAL, while no back-end is
    // connected, so they have to be on before the device is probed and opened. Only the verb is applied
    // here; the speaker/headphone controls still follow after the prefill below, as before.
    if (route_onboard) {
        const ucm::result routes = ucm::apply(mixer_, cfg_, "HiFi", nullptr);
        if (routes.failed()) {
            Leticia::ui_print("audio_manager: ucm verb routes failed: %s", routes.error_description());
            finish("cannot apply the onboard routes");
            return;
        }
    }

    ModernAlsa::pcm_params params;
    if (open_device_with_retry([&] { return params.open(target.card, target.device, false); }).failed()) {
        finish("cannot probe the device");
        return;
    }

    const std::vector<ModernAlsa::sample_format> supported = list_formats(params);
    const bool can_pause = params.supports_pause();
    unsigned int out_rate = av.codec->sample_rate > 0 ? static_cast<unsigned int>(av.codec->sample_rate) : 44100;
    size_t out_channels = target.channels;
    const pcm_format_mapping *fmt_map = nullptr;
    if (!choose_output_config(params, supported, max_bit_depth_, out_rate, out_channels, &fmt_map, &out_rate, &out_channels)) {
        finish("no supported format, rate and channel combination");
        return;
    }

    audio_device_info_t info;
    describe_capabilities(params, supported, info);

    ModernAlsa::size_type period_size = 1024;
    const ModernAlsa::size_type period_count = 4;
    auto min_period = params.get_min_period_size();
    auto max_period = params.get_max_period_size();
    if (!min_period.failed() && !max_period.failed()) {
        const ModernAlsa::size_type lo = min_period.value;
        const ModernAlsa::size_type hi = max_period.value;
        period_size = std::clamp(period_size, lo, hi);
        ModernAlsa::size_type p = 1;
        while (p * 2 <= period_size) p *= 2;
        if (p >= lo) period_size = p;
    }
    params.close();

    AVChannelLayout in_layout;
    if (av.codec->ch_layout.nb_channels > 0)
        av_channel_layout_copy(&in_layout, &av.codec->ch_layout);
    else
        av_channel_layout_default(&in_layout, 1);
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, static_cast<int>(out_channels));

    resampler rs;
    const bool resampler_ok = rs.init(av.codec->sample_rate, in_layout, av.codec->sample_fmt,
                                      static_cast<int>(out_rate), out_layout, fmt_map->av_format);
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);
    if (!resampler_ok) {
        finish("resampler init failed");
        return;
    }

    ModernAlsa::interleaved_pcm_writer writer;
    const auto open_writer = [&] {
        const auto opened = open_device_with_retry([&] { return writer.open(target.card, target.device, false); });
        if (!opened.failed() && !target.via_usb && onboard_reopen_workaround_) writer.set_reopen_before_reconfigure(true);
        return opened;
    };
    if (open_writer().failed()) {
        finish("cannot open the device");
        return;
    }

    ModernAlsa::pcm_config config;
    config.channels = out_channels;
    config.rate = out_rate;
    config.format = fmt_map->alsa_format;

    // The probe above only vetted channels, rate and format. setup() additionally pins the period
    // size and count, and the driver's own hw_params callback may still refuse them (buffer size
    // limits, DMA allocation). So rather than giving up on the first refusal, walk a short list of
    // geometries, reopening the PCM between attempts so each one starts from a clean state. Every
    // failure is logged with its errno, because "device configuration failed" alone says nothing.
    struct geometry {
        ModernAlsa::size_type size;
        ModernAlsa::size_type count;
    };
    const geometry geometries[] = {
        {period_size, period_count},
        {period_size, 2},
        {period_size * 2, 2},
        {period_size / 2, 4},
        {period_size * 4, 2},
    };
    const unsigned int log_card = static_cast<unsigned int>(target.card);
    const unsigned int log_device = static_cast<unsigned int>(target.device);
    Leticia::ui_print("audio_manager: configuring hw:%u,%u as %s, %u Hz, %u ch", log_card, log_device,
                      ModernAlsa::to_string(fmt_map->alsa_format), out_rate, static_cast<unsigned int>(out_channels));

    bool configured = false;
    bool reopen_failed = false;
    for (size_t attempt = 0; attempt < std::size(geometries) && !configured; ++attempt) {
        const geometry &g = geometries[attempt];
        if (g.size == 0) continue;
        if (attempt > 0) {
            writer.close();
            if (open_writer().failed()) {
                reopen_failed = true;
                Leticia::ui_print("audio_manager: hw:%u,%u could not be reopened for another attempt", log_card, log_device);
                break;
            }
        }
        config.period_size = g.size;
        config.period_count = g.count;

        const auto setup_result = writer.setup(config);
        if (setup_result.failed()) {
            Leticia::ui_print("audio_manager: hw:%u,%u setup refused %u x %u frames: %s (errno %d)", log_card, log_device,
                              static_cast<unsigned int>(g.count), static_cast<unsigned int>(g.size),
                              setup_result.error_description(), setup_result.error);
            continue;
        }
        const auto prepare_result = writer.prepare();
        if (prepare_result.failed()) {
            Leticia::ui_print("audio_manager: hw:%u,%u prepare failed after %u x %u frames: %s (errno %d)", log_card,
                              log_device, static_cast<unsigned int>(g.count), static_cast<unsigned int>(g.size),
                              prepare_result.error_description(), prepare_result.error);
            // Unlike a refused setup(), this does not depend on the buffer geometry, so more attempts would
            // only repeat the same failure.
            break;
        }
        configured = true;
        if (attempt > 0) {
            Leticia::ui_print("audio_manager: hw:%u,%u accepted %u x %u frames after %u refused attempt(s)", log_card,
                              log_device, static_cast<unsigned int>(g.count), static_cast<unsigned int>(g.size),
                              static_cast<unsigned int>(attempt));
        }
    }
    if (!configured) {
        writer.close();
        finish(reopen_failed ? "device configuration failed (reopen)" : "device configuration failed");
        return;
    }
    period_size = config.period_size;

    const size_t frame_bytes = out_channels * fmt_map->bytes_per_sample;
    const size_t convert_bytes = out_channels * (fmt_map->av_format == AV_SAMPLE_FMT_S16 ? sizeof(int16_t) : sizeof(int32_t));

    info.card = static_cast<unsigned int>(target.card);
    info.device = static_cast<unsigned int>(target.device);
    info.sample_rate = out_rate;
    info.channels = out_channels;
    info.format = ModernAlsa::to_string(fmt_map->alsa_format);
    info.output = target.via_usb ? "USB DAC" : (onboard_output == audio_output::headphones ? "Headphones" : "Speaker");
    info.name = target.via_usb ? alsa_scan::card_label(static_cast<unsigned int>(target.card)) : cfg_.card_display_name;
    {
        std::lock_guard<std::mutex> lock(device_info_mutex_);
        device_info_ = info;
    }

    {
        std::vector<uint8_t> silence(period_size * frame_bytes, 0);
        ModernAlsa::size_type offset = 0;
        while (offset < period_size) {
            auto w = writer.write_unformatted(silence.data() + offset * frame_bytes, period_size - offset);
            if (w.failed()) break;
            offset += w.value;
        }
    }

    if (route_onboard && !apply_output(onboard_output)) {
        writer.close();
        finish("cannot apply the onboard route");
        return;
    }

    Leticia::ui_print("audio_manager: playing on hw:%u,%u (%s, %u Hz, %zu ch, %s)", static_cast<unsigned int>(target.card),
                      static_cast<unsigned int>(target.device), target.via_usb ? "USB" : "onboard", out_rate, out_channels,
                      ModernAlsa::to_string(fmt_map->alsa_format));

    std::vector<uint8_t> convert_buffer;
    std::vector<uint8_t> pack_buffer;
    write_status status = write_status::ok;

    const bool convert_is_16bit = fmt_map->av_format == AV_SAMPLE_FMT_S16;
    double applied_gain = volume_gain(volume());

    auto deliver = [&](int frames) {
        if (frames <= 0) return write_status::ok;

        const double target_gain = volume_gain(volume());
        if (applied_gain != 1.0 || target_gain != 1.0) {
            if (convert_is_16bit)
                scale_samples(reinterpret_cast<int16_t *>(convert_buffer.data()), static_cast<size_t>(frames), out_channels,
                              applied_gain, target_gain);
            else
                scale_samples(reinterpret_cast<int32_t *>(convert_buffer.data()), static_cast<size_t>(frames), out_channels,
                              applied_gain, target_gain);
        }
        applied_gain = target_gain;

        const uint8_t *data = convert_buffer.data();
        if (fmt_map->post_shift != 0) {
            auto *samples = reinterpret_cast<int32_t *>(convert_buffer.data());
            const size_t count = static_cast<size_t>(frames) * out_channels;
            for (size_t i = 0; i < count; i++) samples[i] >>= fmt_map->post_shift;
        } else if (fmt_map->packed_24) {
            const size_t count = static_cast<size_t>(frames) * out_channels;
            pack_buffer.resize(count * 3);
            pack_s24_3le(reinterpret_cast<const int32_t *>(convert_buffer.data()), count, pack_buffer.data());
            data = pack_buffer.data();
        }
        return write_frames(writer, data, static_cast<ModernAlsa::size_type>(frames), frame_bytes, should_stop_);
    };

    enum class pause_mode { none, hardware, dropped };
    pause_mode pause_state = pause_mode::none;
    bool end_of_file = false;

    while (!should_stop_.load(std::memory_order_acquire) && status == write_status::ok && !end_of_file) {
        const int64_t seek_seconds = seek_request_seconds_.exchange(-1, std::memory_order_acq_rel);
        if (seek_seconds >= 0) {
            const int64_t timestamp = av_rescale_q(seek_seconds, AVRational{1, 1}, stream->time_base);
            if (av_seek_frame(av.format, av.stream_index, timestamp, AVSEEK_FLAG_BACKWARD) >= 0) {
                avcodec_flush_buffers(av.codec);
                writer.drop();
                writer.prepare();
                if (pause_state == pause_mode::hardware) pause_state = pause_mode::dropped;
                position_seconds_.store(static_cast<uint64_t>(seek_seconds), std::memory_order_release);
            }
        }

        if (paused_.load(std::memory_order_acquire)) {
            if (pause_state == pause_mode::none) {
                if (can_pause && !writer.pause(true).failed()) {
                    pause_state = pause_mode::hardware;
                } else {
                    writer.drop();
                    writer.prepare();
                    pause_state = pause_mode::dropped;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (pause_state != pause_mode::none) {
            if (pause_state == pause_mode::hardware && writer.pause(false).failed()) writer.prepare();
            if (pause_state == pause_mode::dropped) writer.prepare();
            pause_state = pause_mode::none;
        }

        if (av_read_frame(av.format, av.packet) < 0) {
            end_of_file = true;
            avcodec_send_packet(av.codec, nullptr);
        } else if (av.packet->stream_index != av.stream_index) {
            av_packet_unref(av.packet);
            continue;
        } else {
            const int sent = avcodec_send_packet(av.codec, av.packet);
            av_packet_unref(av.packet);
            if (sent < 0 && sent != AVERROR(EAGAIN)) continue;
        }

        while (status == write_status::ok && avcodec_receive_frame(av.codec, av.frame) == 0) {
            if (av.frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                const int64_t position = av_rescale_q(av.frame->best_effort_timestamp, stream->time_base, AVRational{1, 1});
                if (position >= 0) position_seconds_.store(static_cast<uint64_t>(position), std::memory_order_release);
            }

            const int max_out = rs.max_output_samples(av.frame->nb_samples);
            convert_buffer.resize(static_cast<size_t>(max_out) * convert_bytes);
            uint8_t *out_ptr = convert_buffer.data();
            const int converted = rs.convert(const_cast<const uint8_t **>(av.frame->extended_data), av.frame->nb_samples, &out_ptr, max_out);
            av_frame_unref(av.frame);
            status = deliver(converted);
        }
    }

    if (end_of_file && status == write_status::ok && !should_stop_.load(std::memory_order_acquire)) {
        const int max_out = rs.max_output_samples(0);
        convert_buffer.resize(static_cast<size_t>(max_out) * convert_bytes);
        uint8_t *out_ptr = convert_buffer.data();
        status = deliver(rs.flush(&out_ptr, max_out));
    }

    const bool completed = end_of_file && status == write_status::ok && !should_stop_.load(std::memory_order_acquire);
    if (completed)
        writer.drain();
    else
        writer.drop();

    if (route_onboard) mute_output();
    writer.close();

    if (status == write_status::disconnected) {
        Leticia::ui_print("audio_manager: playback device was disconnected");
        resume_pending_.store(true, std::memory_order_release);
        auto_paused_.store(true, std::memory_order_release);
    } else if (completed) {
        const music_track_t *track = current_track();
        if (track != nullptr && track->duration_seconds != 0)
            position_seconds_.store(track->duration_seconds, std::memory_order_release);
        track_finished_.store(true, std::memory_order_release);
    }

    finish(nullptr);
}

} // namespace Leticia
