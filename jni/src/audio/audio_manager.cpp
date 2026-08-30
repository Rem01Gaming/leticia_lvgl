#include "audio_manager.hpp"
#include "util/updater_proto.hpp"

#include <cmath>
#include <cstdio>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace Leticia {

namespace {

constexpr size_t kSampleRate = 48000;
constexpr double kToneFrequencyHz = 1000.0;
constexpr double kToneAmplitude = 0.3 * 32767.0;
constexpr double kTwoPi = 6.283185307179586;
constexpr uint32_t kJackPollIntervalMs = 250;

const char *device_name_for(audio_output out) {
    switch (out) {
        case audio_output::speaker: return "Speaker";
        case audio_output::headphones: return "Headphones";
        case audio_output::unknown: return nullptr;
    }
    return nullptr;
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

} // namespace

audio_manager::~audio_manager() {
    deinit();
}

audio_output audio_manager::detected_output() const {
    if (input_monitor_.headphone_state_known() && input_monitor_.is_headphone_connected())
        return audio_output::headphones;
    return audio_output::speaker;
}

bool audio_manager::init(const device_config_t &device_config, const std::string &zip_path) {
    card_ = device_config.alsa_card;
    cpu_affinity_ = device_config.audio_thread_affinity;

    std::string ucm_text;
    if (load_alsa_ucm_config_text(zip_path, ucm_text)) {
        ucm::parse_error err = cfg_.parse(ucm_text.c_str());
        if (err.failed()) {
            Leticia::ui_print("audio_manager: UCM config parse error: %s (line %u)", err.description(), err.line);
        } else {
            cfg_loaded_ = true;
            Leticia::ui_print("audio_manager: loaded UCM config for card '%s' (%s)", cfg_.card_name, cfg_.card_display_name);
        }
    } else {
        Leticia::ui_print("audio_manager: no UCM config found, audio test unavailable");
    }

    if (cfg_loaded_) {
        ModernAlsa::result r = mixer_.open(card_);
        if (r.failed()) {
            Leticia::ui_print("audio_manager: could not open mixer for card %u: %s", card_, r.error_description());
            cfg_loaded_ = false;
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

    return is_available();
}

void audio_manager::deinit() {
    stop_test_tone();

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

    // If tone is playing, apply new routing from the main thread context
    // The audio thread will pick up the change
    if (tone_playing_.load(std::memory_order_acquire)) {
        // We need to apply mixer changes from the main thread since
        // ALSA mixer operations aren't typically thread-safe with PCM
        apply_output(new_output);
    }
}

void audio_manager::audio_thread_func() {
    apply_audio_cpu_affinity(cpu_affinity_);

    Leticia::ui_print("audio_manager: audio thread started");

    while (!should_stop_.load(std::memory_order_acquire)) {
        // Wait for tone to start or stop signal
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait_for(lock, std::chrono::milliseconds(10), [this] {
            return should_stop_.load(std::memory_order_acquire) ||
                   tone_playing_.load(std::memory_order_acquire);
        });

        if (should_stop_.load(std::memory_order_acquire))
            break;

        if (!tone_playing_.load(std::memory_order_acquire))
            continue;

        // Fill audio buffer while tone is playing
        fill_buffer();
    }

    // Clean up PCM on thread exit
    if (pcm_.is_open()) {
        pcm_.drop();
        pcm_.close();
    }

    Leticia::ui_print("audio_manager: audio thread stopped");
}

bool audio_manager::start_test_tone() {
    bool expected = false;
    if (!tone_playing_.compare_exchange_strong(expected, true)) {
        return true;  // Already playing
    }

    if (!is_available()) {
        Leticia::ui_print("audio_manager: not available, cannot start test tone");
        tone_playing_.store(false, std::memory_order_release);
        return false;
    }

    /**
     * @brief Synchronizes the mixer with the headphone jack.
     */
    if (!apply_output(detected_output_)) {
        Leticia::ui_print("audio_manager: could not apply routing, aborting test tone");
        tone_playing_.store(false, std::memory_order_release);
        return false;
    }

    size_t device_index = 0;
    const ucm::verb *v = cfg_.find_verb("HiFi");
    if (v != nullptr && v->playback_pcm_device != ucm::invalid_index()) {
        device_index = v->playback_pcm_device;
    } else {
        Leticia::ui_print("audio_manager: no playback_pcm declared in UCM config, assuming device 0");
    }

    size_t channels = kMaxChannels;
    const ucm::device *dev = (v != nullptr) ? v->find_device(device_name_for(detected_output_)) : nullptr;
    if (dev != nullptr && dev->channels != 0) {
        if (dev->channels > kMaxChannels) {
            Leticia::ui_print("audio_manager: device declares %zu channels, clamping to %zu",
                              dev->channels,
                              kMaxChannels);
        } else {
            channels = dev->channels;
        }
    }

    /**
     * @brief Probes PCM parameters.
     */
    {
        ModernAlsa::pcm_params params;
        ModernAlsa::result pr = params.open(card_, device_index, false);
        if (!pr.failed()) {
            if (!params.test_config(channels, kSampleRate, ModernAlsa::sample_format::s16_le)) {
                Leticia::ui_print(
                        "audio_manager: pcm %u,%zu may not support %zu ch / %zu Hz / s16_le, attempting anyway",
                        card_,
                        device_index,
                        channels,
                        kSampleRate);
            }
            params.close();
        }
    }

    ModernAlsa::result r = pcm_.open(card_, device_index, true);
    if (r.failed()) {
        Leticia::ui_print("audio_manager: pcm open failed: %s", r.error_description());
        tone_playing_.store(false, std::memory_order_release);
        return false;
    }

    ModernAlsa::pcm_config config;
    config.channels = channels;
    config.rate = kSampleRate;
    config.format = ModernAlsa::sample_format::s16_le;
    config.disable_resampling = true;

    r = pcm_.setup(config);
    if (r.failed()) {
        Leticia::ui_print("audio_manager: pcm setup failed: %s", r.error_description());
        pcm_.close();
        tone_playing_.store(false, std::memory_order_release);
        return false;
    }

    r = pcm_.prepare();
    if (r.failed()) {
        Leticia::ui_print("audio_manager: pcm prepare failed: %s", r.error_description());
        pcm_.close();
        tone_playing_.store(false, std::memory_order_release);
        return false;
    }

    pcm_channels_ = channels;
    phase_ = 0.0;

    // Start the audio thread if not already running
    if (!thread_running_.load(std::memory_order_acquire)) {
        should_stop_.store(false, std::memory_order_release);
        thread_running_.store(true, std::memory_order_release);
        audio_thread_ = std::thread(&audio_manager::audio_thread_func, this);
    }

    // Notify the audio thread to start filling
    cv_.notify_one();

    Leticia::ui_print("audio_manager: playing 1kHz test tone (%zu ch, %zu Hz, s16_le) on thread",
                      channels, kSampleRate);
    return true;
}

void audio_manager::stop_test_tone() {
    bool expected = true;
    if (!tone_playing_.compare_exchange_strong(expected, false)) {
        return;
    }

    Leticia::ui_print("audio_manager: stopping test tone");

    if (thread_running_.load(std::memory_order_acquire)) {
        should_stop_.store(true, std::memory_order_release);
        cv_.notify_one();
        if (audio_thread_.joinable()) {
            audio_thread_.join();
        }
        thread_running_.store(false, std::memory_order_release);
        should_stop_.store(false, std::memory_order_release);
    }

    Leticia::ui_print("audio_manager: test tone stopped");
}

bool audio_manager::toggle_test_tone() {
    if (tone_playing_.load(std::memory_order_acquire)) {
        stop_test_tone();
        return false;
    }
    return start_test_tone();
}

void audio_manager::fill_buffer() {
    if (!pcm_.is_open() || pcm_channels_ == 0)
        return;

    ModernAlsa::generic_result<ModernAlsa::size_type> avail = pcm_.get_avail();
    if (avail.failed())
        return;

    size_t frames = avail.value;
    if (frames == 0)
        return;

    size_t max_frames = kMaxFillFrames / pcm_channels_;
    if (frames > max_frames)
        frames = max_frames;

    double phase_increment = kTwoPi * kToneFrequencyHz / static_cast<double>(kSampleRate);

    for (size_t i = 0; i < frames; i++) {
        auto sample = static_cast<int16_t>(std::lround(std::sin(phase_) * kToneAmplitude));
        for (size_t ch = 0; ch < pcm_channels_; ch++)
            fill_buf_[i * pcm_channels_ + ch] = sample;

        phase_ += phase_increment;
        if (phase_ >= kTwoPi)
            phase_ -= kTwoPi;
    }

    ModernAlsa::generic_result<ModernAlsa::size_type> written = pcm_.write_unformatted(fill_buf_.data(), frames);
    if (written.failed()) {
        Leticia::ui_print("audio_manager: pcm write failed: %s", written.error_description());
        tone_playing_.store(false, std::memory_order_release);
    }
}

void audio_manager::poll_timer_trampoline(lv_timer_t *timer) {
    auto *self = static_cast<audio_manager *>(lv_timer_get_user_data(timer));
    self->input_monitor_.poll();
    }

} // namespace Leticia
