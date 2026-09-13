#include "audio_manager.hpp"
#include "resampler.hpp"
#include "util/updater_proto.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

namespace Leticia {

namespace {

constexpr uint32_t kJackPollIntervalMs = 250;
constexpr size_t kFallbackChannels = 2;

const char *device_name_for(audio_output out) {
    switch (out) {
        case audio_output::speaker: return "Speaker";
        case audio_output::headphones: return "Headphones";
        case audio_output::unknown: return nullptr;
    }
    return nullptr;
}

/**
 * @brief Joins a directory and a file name with exactly one separating slash.
 */
std::string join_path(const std::string &dir, const std::string &file) {
    if (dir.empty()) return file;
    if (dir.back() == '/') return dir + file;
    return dir + "/" + file;
}

/**
 * @brief Applies CPU affinity to the current thread.
 */
void apply_audio_cpu_affinity(const std::vector<int> &cpus) {
    if (cpus.empty()) {
        Leticia::ui_print("apply_audio_cpu_affinity: no audio thread affinity specified, skipping thread pin");
        return;
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    for (int cpu_id : cpus) {
        if (cpu_id < 0 || cpu_id >= CPU_SETSIZE) {
            Leticia::ui_print("apply_audio_cpu_affinity: CPU ID %d out of range [0-%d]", cpu_id, CPU_SETSIZE - 1);
            return;
        }
        CPU_SET(cpu_id, &cpuset);
    }

    if (sched_setaffinity(syscall(SYS_gettid), sizeof(cpuset), &cpuset) != 0) {
        Leticia::ui_print("apply_audio_cpu_affinity: sched_setaffinity failed: %s", strerror(errno));
        return;
    }

    std::string cpu_list;
    for (size_t i = 0; i < cpus.size(); i++) {
        if (i > 0) cpu_list += ", ";
        cpu_list += std::to_string(cpus[i]);
    }
    Leticia::ui_print("apply_audio_cpu_affinity: audio thread pinned to CPU(s): %s", cpu_list.c_str());
}

/**
 * @brief Where playback should be routed, resolved once before decoding starts.
 */
struct output_target {
    ModernAlsa::size_type card = 0;
    ModernAlsa::size_type device = 0;
    size_t channels = kFallbackChannels;
    bool via_usb = false;
};

/**
 * @brief Pairs an ALSA sample format with the FFmpeg format that produces an
 * identical (or trivially post-processable) in-memory sample layout, so the
 * two can never drift apart.
 */
struct pcm_format_mapping {
    ModernAlsa::sample_format alsa_format;
    AVSampleFormat av_format;
    size_t bytes_per_sample;

    /// Significant bits actually carried in this format, for
    /// device_config_t::audio_max_bit_depth filtering. Distinct from
    /// bytes_per_sample * 8: S24_LE is 4 bytes wide but only 24 bits
    /// significant.
    unsigned int valid_bits;

    /// Arithmetic right-shift to apply to each converted S32 sample before
    /// writing, so the significant bits land where this alsa_format expects
    /// them. 0 for formats the resampler already produces in final form.
    unsigned int post_shift;
};

constexpr pcm_format_mapping kSupportedFormats[] = {
    {ModernAlsa::sample_format::s32_le, AV_SAMPLE_FMT_S32, sizeof(int32_t), 32, 0},
    {ModernAlsa::sample_format::s24_le, AV_SAMPLE_FMT_S32, sizeof(int32_t), 24, 8},
    {ModernAlsa::sample_format::s16_le, AV_SAMPLE_FMT_S16, sizeof(int16_t), 16, 0},
};

/**
 * @brief Picks the most preferred ALSA/FFmpeg format pair the device
 * supports, not exceeding max_bit_depth.
 *
 * @param supported Formats the device reported as supported, in any order.
 * @param max_bit_depth Skip any candidate whose valid_bits exceeds this.
 * @return Pointer into kSupportedFormats, or nullptr if none match.
 */
const pcm_format_mapping *pick_output_format(const std::vector<ModernAlsa::sample_format> &supported,
                                              unsigned int max_bit_depth) {
    for (const auto &candidate : kSupportedFormats) {
        if (max_bit_depth != 0 && candidate.valid_bits > max_bit_depth)
            continue;
        if (std::find(supported.begin(), supported.end(), candidate.alsa_format) != supported.end())
            return &candidate;
    }
    return nullptr;
}

/**
 * @brief Finds the first USB-attached playback PCM.
 *
 * @return true if a USB playback device was found, false otherwise.
 */
bool find_usb_playback_device(ModernAlsa::size_type &out_card, ModernAlsa::size_type &out_device) {
    ModernAlsa::pcm_list list;
    for (ModernAlsa::size_type i = 0; i < list.size(); i++) {
        const auto &info = list[i];
        if (info.is_capture) continue;
        if (info.card == 0) continue;

        out_card = info.card;
        out_device = info.device;
        return true;
    }

    if (list.size() == 0) {
        Leticia::ui_print("audio_manager: pcm_list opened no PCM devices at all (not even onboard card 0)");
    } else {
        for (ModernAlsa::size_type i = 0; i < list.size(); i++) {
            const auto &info = list[i];
            Leticia::ui_print("audio_manager: pcm_list saw card %u device %u (%s, %s)",
                              (unsigned)info.card, (unsigned)info.device,
                              info.is_capture ? "capture" : "playback", info.name);
        }
    }

    return false;
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
                          const std::string &zip_path) {
    card_ = device_config.alsa_card;
    cpu_affinity_ = device_config.audio_thread_affinity;
    onboard_reopen_workaround_ = device_config.onboard_audio_reopen_workaround;
    max_bit_depth_ = device_config.audio_max_bit_depth;
    music_file_path_ = join_path(user_config.music_scan_path, "music.flac");

    std::string ucm_path;
    if (resolve_alsa_ucm_config_path(zip_path, ucm_path)) {
        ucm::parse_error err = cfg_.parse_file(ucm_path.c_str());
        if (err.failed()) {
            Leticia::ui_print("audio_manager: UCM config parse error: %s (line %u)", err.description(), err.line);
        } else {
            cfg_loaded_ = true;
            Leticia::ui_print("audio_manager: loaded UCM config from %s for card '%s' (%s)",
                              ucm_path.c_str(), cfg_.card_name, cfg_.card_display_name);
        }
    } else {
        Leticia::ui_print("audio_manager: no UCM config found, onboard routing unavailable");
    }

    if (cfg_loaded_) {
        ModernAlsa::result r = mixer_.open(card_);
        if (r.failed()) {
            Leticia::ui_print("audio_manager: could not open mixer for card %u: %s", card_, r.error_description());
            cfg_loaded_ = false;
        }

        auto info = mixer_.get_card_info();
        if (!info.failed() && strstr(info.value.name, cfg_.card_name) == nullptr) {
            Leticia::ui_print("audio_manager: mixer reports '%s', config expects '%s', possible board mismatch",
                              info.value.name, cfg_.card_name);
        }
    }

    if (input_monitor_.open()) {
        input_monitor_.set_callback([this](const input_event_t &event) {
            on_input_event(event);
        });
    } else {
        Leticia::ui_print("audio_manager: no input sources found, jack auto-detect unavailable (assuming speaker)");
    }

    detected_output_ = detected_output();
    poll_timer_ = lv_timer_create(poll_timer_trampoline, kJackPollIntervalMs, this);

    Leticia::ui_print("audio_manager: initial output guess: %s",
                      detected_output_ == audio_output::headphones ? "Headphones" : "Speaker");
    Leticia::ui_print("audio_manager: music file set to %s", music_file_path_.c_str());

    return is_available();
}

void audio_manager::deinit() {
    should_stop_.store(true, std::memory_order_release);
    if (playback_thread_.joinable()) {
        playback_thread_.join();
    }
    playing_.store(false, std::memory_order_release);

    if (poll_timer_ != nullptr) {
        lv_timer_delete(poll_timer_);
        poll_timer_ = nullptr;
    }

    input_monitor_.close();
    mixer_.close();
    cfg_loaded_ = false;
}

bool audio_manager::apply_output(audio_output target) {
    if (!cfg_loaded_ || !mixer_.is_open())
        return false;

    const char *new_name = device_name_for(target);
    if (new_name == nullptr)
        return false;

    const char *prev_name = device_name_for(mixer_applied_output_);

    ucm::result r = ucm::apply(mixer_, cfg_, "HiFi", new_name, prev_name);
    if (r.failed()) {
        Leticia::ui_print("audio_manager: ucm::apply(HiFi, %s) failed: %s", new_name, r.error_description());
        return false;
    }

    Leticia::ui_print("audio_manager: routed to %s", new_name);
    mixer_applied_output_ = target;
    return true;
}

void audio_manager::on_input_event(const input_event_t &event) {
    if (event.type != input_event_type::headphone_insert && event.type != input_event_type::headphone_remove)
        return;

    audio_output new_output = detected_output();
    detected_output_ = new_output;

    if (playing_.load(std::memory_order_acquire)) {
        apply_output(new_output);
    }
}

bool audio_manager::start_playback() {
    bool expected = false;
    if (!playing_.compare_exchange_strong(expected, true)) {
        return true; // Already playing
    }

    if (playback_thread_.joinable()) {
        playback_thread_.join();
    }

    if (music_file_path_.empty()) {
        Leticia::ui_print("audio_manager: no music file configured, cannot play");
        playing_.store(false, std::memory_order_release);
        return false;
    }

    should_stop_.store(false, std::memory_order_release);
    playback_thread_ = std::thread(&audio_manager::playback_thread_func, this);
    return true;
}

void audio_manager::stop_playback() {
    should_stop_.store(true, std::memory_order_release);
    if (playback_thread_.joinable()) {
        playback_thread_.join();
    }
    playing_.store(false, std::memory_order_release);
}

bool audio_manager::toggle_playback() {
    if (playing_.load(std::memory_order_acquire)) {
        stop_playback();
        return false;
    }
    return start_playback();
}

void audio_manager::playback_thread_func() {
    apply_audio_cpu_affinity(cpu_affinity_);
    Leticia::ui_print("audio_manager: playback thread started (%s)", music_file_path_.c_str());

    output_target target;
    if (find_usb_playback_device(target.card, target.device)) {
        target.channels = kFallbackChannels;
        target.via_usb = true;
        Leticia::ui_print("audio_manager: USB DAC detected on card %u, routing there",
                          (unsigned)target.card);
    } else if (cfg_loaded_ && mixer_.is_open()) {
        audio_output onboard = detected_output();
        if (!apply_output(onboard)) {
            Leticia::ui_print("audio_manager: could not apply onboard routing, aborting playback");
            playing_.store(false, std::memory_order_release);
            return;
        }

        target.card = card_;
        target.device = 0;
        const ucm::verb *v = cfg_.find_verb("HiFi");
        if (v != nullptr && v->playback_pcm_device != ucm::invalid_index()) {
            target.device = v->playback_pcm_device;
        } else {
            Leticia::ui_print("audio_manager: no playback_pcm declared in UCM config, assuming device 0");
        }

        target.channels = kFallbackChannels;
        const ucm::device *dev = (v != nullptr) ? v->find_device(device_name_for(onboard)) : nullptr;
        if (dev != nullptr && dev->channels != 0) {
            target.channels = dev->channels;
        }
    } else {
        Leticia::ui_print("audio_manager: no USB DAC and no UCM config, cannot play");
        playing_.store(false, std::memory_order_release);
        return;
    }

    AVFormatContext *fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, music_file_path_.c_str(), nullptr, nullptr) < 0) {
        Leticia::ui_print("audio_manager: cannot open %s", music_file_path_.c_str());
        playing_.store(false, std::memory_order_release);
        return;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        Leticia::ui_print("audio_manager: cannot find stream info for %s", music_file_path_.c_str());
        avformat_close_input(&fmt_ctx);
        playing_.store(false, std::memory_order_release);
        return;
    }

    int audio_idx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_idx = (int)i;
            break;
        }
    }

    if (audio_idx < 0) {
        Leticia::ui_print("audio_manager: no audio stream in %s", music_file_path_.c_str());
        avformat_close_input(&fmt_ctx);
        playing_.store(false, std::memory_order_release);
        return;
    }

    AVStream *stream = fmt_ctx->streams[audio_idx];
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        Leticia::ui_print("audio_manager: no decoder for codec id %d", (int)stream->codecpar->codec_id);
        avformat_close_input(&fmt_ctx);
        playing_.store(false, std::memory_order_release);
        return;
    }

    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, stream->codecpar);
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        Leticia::ui_print("audio_manager: cannot open codec for %s", music_file_path_.c_str());
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        playing_.store(false, std::memory_order_release);
        return;
    }

    int out_rate = codec_ctx->sample_rate ? codec_ctx->sample_rate : 44100;
    size_t out_channels = target.channels;

    ModernAlsa::pcm_params params;
    if (params.open(target.card, target.device, false).failed()) {
        Leticia::ui_print("audio_manager: cannot open pcm params for hw:%u,%u",
                          (unsigned)target.card, (unsigned)target.device);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        playing_.store(false, std::memory_order_release);
        return;
    }

    std::vector<ModernAlsa::sample_format> supported_fmts;
    params.for_each_supported_format(
        [](ModernAlsa::sample_format fmt, void *ud) {
            static_cast<std::vector<ModernAlsa::sample_format> *>(ud)->push_back(fmt);
        },
        &supported_fmts
    );

    const pcm_format_mapping *fmt_map = pick_output_format(supported_fmts, max_bit_depth_);
    if (fmt_map == nullptr) {
        Leticia::ui_print("audio_manager: device has no S32/S24/S16 playback format we can output bit-perfect"
                          "%s", max_bit_depth_ != 0 ? " within the configured audio_max_bit_depth" : "");
        params.close();
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        playing_.store(false, std::memory_order_release);
        return;
    }

    if (!params.test_channels(out_channels)) {
        Leticia::ui_print("audio_manager: device does not support %zu channels", out_channels);
        params.close();
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        playing_.store(false, std::memory_order_release);
        return;
    }

    if (!params.test_rate((unsigned)out_rate)) {
        Leticia::ui_print("audio_manager: device does not support rate %d", out_rate);
        params.close();
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        playing_.store(false, std::memory_order_release);
        return;
    }

    ModernAlsa::size_type period_size = 1024;
    ModernAlsa::size_type period_count = 4;

    auto min_period = params.get_min_period_size();
    auto max_period = params.get_max_period_size();
    if (!min_period.failed() && !max_period.failed()) {
        ModernAlsa::size_type lo = min_period.unwrap();
        ModernAlsa::size_type hi = max_period.unwrap();

        if (period_size < lo) period_size = lo;
        if (period_size > hi) period_size = hi;

        // Round down to a power of two within range (most drivers prefer this)
        ModernAlsa::size_type p = 1;
        while (p * 2 <= period_size) p *= 2;
        if (p >= lo) period_size = p;
    }

    params.close();

    resampler rs;
    AVChannelLayout in_layout;
    if (codec_ctx->ch_layout.nb_channels > 0) {
        av_channel_layout_copy(&in_layout, &codec_ctx->ch_layout);
    } else {
        av_channel_layout_default(&in_layout, 1);
    }

    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, (int)out_channels);

    if (!rs.init(codec_ctx->sample_rate, in_layout, codec_ctx->sample_fmt, out_rate, out_layout, fmt_map->av_format)) {
        Leticia::ui_print("audio_manager: resampler init failed");
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        playing_.store(false, std::memory_order_release);
        return;
    }

    ModernAlsa::interleaved_pcm_writer writer;
    if (writer.open(target.card, target.device, false).failed()) {
        Leticia::ui_print("audio_manager: cannot open pcm device hw:%u,%u",
                          (unsigned)target.card, (unsigned)target.device);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        playing_.store(false, std::memory_order_release);
        return;
    }

    if (!target.via_usb && onboard_reopen_workaround_) {
        writer.set_reopen_before_reconfigure(true);
    }

    ModernAlsa::pcm_config config;
    config.channels = out_channels;
    config.rate = (unsigned)out_rate;
    config.format = fmt_map->alsa_format;
    config.period_size = period_size;
    config.period_count = period_count;

    if (writer.setup(config).failed() || writer.prepare().failed()) {
        Leticia::ui_print("audio_manager: pcm configuration failed for hw:%u,%u",
                          (unsigned)target.card, (unsigned)target.device);
        writer.close();
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        playing_.store(false, std::memory_order_release);
        return;
    }

    Leticia::ui_print("audio_manager: playing %s on hw:%u,%u (%s, %d Hz, %zu ch, %s)",
                      music_file_path_.c_str(), (unsigned)target.card, (unsigned)target.device,
                      target.via_usb ? "USB" : "onboard", out_rate, out_channels,
                      ModernAlsa::to_string(fmt_map->alsa_format));

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    const size_t frame_bytes = out_channels * fmt_map->bytes_per_sample;
    std::vector<uint8_t> pcm_buf;
    bool device_disconnected = false;

    while (!should_stop_.load(std::memory_order_acquire)) {
        if (av_read_frame(fmt_ctx, pkt) < 0) break;

        if (pkt->stream_index != audio_idx) {
            av_packet_unref(pkt);
            continue;
        }

        if (avcodec_send_packet(codec_ctx, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);

        while (avcodec_receive_frame(codec_ctx, frame) == 0) {
            if (should_stop_.load(std::memory_order_acquire)) {
                av_frame_unref(frame);
                break;
            }

            int max_out = rs.get_delay(out_rate) + frame->nb_samples + 64;
            pcm_buf.resize((size_t)max_out * frame_bytes);

            uint8_t *out_ptr = pcm_buf.data();
            int converted = rs.convert((const uint8_t **)frame->data, frame->nb_samples, &out_ptr, max_out);
            av_frame_unref(frame);
            if (converted <= 0) continue;

            if (fmt_map->post_shift != 0) {
                auto *samples = reinterpret_cast<int32_t *>(pcm_buf.data());
                size_t sample_count = (size_t)converted * out_channels;
                for (size_t i = 0; i < sample_count; i++) {
                    samples[i] >>= fmt_map->post_shift;
                }
            }

            ModernAlsa::size_type written = 0;
            while (written < (ModernAlsa::size_type)converted && !should_stop_.load(std::memory_order_acquire)) {
                auto avail_res = writer.get_avail();
                if (avail_res.failed()) {
                    ModernAlsa::pcm_recover(writer, avail_res.error);
                    if (writer.get_state().unwrap() == ModernAlsa::pcm_state::disconnected) {
                        device_disconnected = true;
                        break;
                    }
                    continue;
                }

                ModernAlsa::size_type avail = avail_res.unwrap();
                if (avail == 0) {
                    auto state = writer.get_state().unwrap();
                    if (state == ModernAlsa::pcm_state::disconnected) {
                        device_disconnected = true;
                        break;
                    } else if (state == ModernAlsa::pcm_state::xrun) {
                        writer.prepare();
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }

                ModernAlsa::size_type to_write = std::min(avail, (ModernAlsa::size_type)(converted - written));
                auto write_res = writer.write_unformatted(pcm_buf.data() + written * frame_bytes, to_write);
                if (write_res.failed()) {
                    ModernAlsa::pcm_recover(writer, write_res.error);
                    if (writer.get_state().unwrap() == ModernAlsa::pcm_state::disconnected) {
                        device_disconnected = true;
                        break;
                    }
                } else {
                    written += write_res.unwrap();
                }
            }

            if (device_disconnected) break;
        }

        if (device_disconnected) break;
    }

    writer.drain();
    writer.close();
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);

    if (device_disconnected) {
        Leticia::ui_print("audio_manager: playback device was disconnected");
    } else {
        Leticia::ui_print("audio_manager: playback finished (%s)", music_file_path_.c_str());
    }

    playing_.store(false, std::memory_order_release);
}

void audio_manager::poll_timer_trampoline(lv_timer_t *timer) {
    auto *self = static_cast<audio_manager *>(lv_timer_get_user_data(timer));
    self->input_monitor_.poll();
}

} // namespace Leticia
