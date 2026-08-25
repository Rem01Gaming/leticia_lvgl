#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include <lvgl.h>

#include "ModernAlsa.hpp"
#include "config/device_config.hpp"
#include "input/input_event.hpp"
#include "alsa_ucm.hpp"

namespace Leticia {

/**
 * @brief Enumeration of audio output paths.
 */
enum class audio_output {
    unknown,    /**< Unknown state. */
    speaker,    /**< Speaker output. */
    headphones, /**< Headphone output. */
};

/**
 * @brief Manages audio routing and test tones.
 */
class audio_manager final {
public:
    audio_manager() = default;
    ~audio_manager();

    audio_manager(const audio_manager &) = delete;
    audio_manager &operator=(const audio_manager &) = delete;

    /**
     * @brief Initializes the audio manager.
     *
     * @param device_config Device configuration.
     * @param zip_path Path to the OTA zip file.
     * @return true if initialized successfully, false otherwise.
     */
    bool init(const device_config_t &device_config, const std::string &zip_path);

    /**
     * @brief Deinitializes the audio manager.
     */
    void deinit();

    /**
     * @brief Checks if audio services are available.
     *
     * @return true if available, false otherwise.
     */
    bool is_available() const {
        return cfg_loaded_ && mixer_.is_open();
    }

    /**
     * @brief Starts the 1kHz test tone.
     *
     * @return true if started successfully, false otherwise.
     */
    bool start_test_tone();

    /**
     * @brief Stops the test tone.
     */
    void stop_test_tone();

    /**
     * @brief Checks if the test tone is playing.
     *
     * @return true if playing, false otherwise.
     */
    bool is_test_tone_playing() const {
        return tone_playing_;
    }

    /**
     * @brief Toggles the test tone playback.
     *
     * @return New playback state.
     */
    bool toggle_test_tone();

    /**
     * @brief Detects the current audio output.
     *
     * @return Detected audio output.
     */
    audio_output detected_output() const;

private:
    ucm::config cfg_;
    bool cfg_loaded_ = false;

    ModernAlsa::mixer mixer_;
    unsigned int card_ = 0;

    input_event_monitor input_monitor_;

    /// What the jack currently indicates, updated on every insert/remove
    /// regardless of whether a tone is playing.
    audio_output detected_output_ = audio_output::unknown;

    /// What the mixer was last actually set to via ucm::apply()
    audio_output mixer_applied_output_ = audio_output::unknown;

    ModernAlsa::interleaved_pcm_writer pcm_;
    bool tone_playing_ = false;
    size_t pcm_channels_ = 0;
    double phase_ = 0.0;
    lv_timer_t *fill_timer_ = nullptr;
    lv_timer_t *poll_timer_ = nullptr;

    static constexpr size_t kMaxFillFrames = 4096;
    static constexpr size_t kMaxChannels = 2;
    std::array<int16_t, kMaxFillFrames * kMaxChannels> fill_buf_{};

    /**
     * @brief Applies audio routing to the target output.
     *
     * @param target Target audio output.
     * @return true if applied successfully, false otherwise.
     */
    bool apply_output(audio_output target);

    void on_input_event(const input_event_t &event);
    void fill_buffer();

    static void fill_timer_trampoline(lv_timer_t *timer);
    static void poll_timer_trampoline(lv_timer_t *timer);
};

} // namespace Leticia
