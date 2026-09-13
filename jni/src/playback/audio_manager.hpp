#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <lvgl.h>

#include "ModernAlsa.hpp"
#include "config/device_config.hpp"
#include "config/user_config.hpp"
#include "input/input_event.hpp"
#include "alsa_ucm.hpp"

namespace Leticia {

/**
 * @brief Enumeration of onboard audio output paths.
 *
 * Only meaningful for the mainboard codec (card 0), which is the only
 * output the UCM config knows how to route. A USB DAC is addressed
 * directly by card/device and never goes through this enum.
 */
enum class audio_output {
    unknown,    /**< Unknown state. */
    speaker,    /**< Speaker output. */
    headphones, /**< Headphone output. */
};

/**
 * @brief Manages audio routing and playback of a fixed music file.
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
     * @param user_config User configuration; music_scan_path is used to locate music.flac.
     * @param zip_path Path to the OTA zip file.
     * @return true if the onboard UCM config loaded, false otherwise. A false return
     * does not rule out playback: a USB DAC can still be used, see start_playback().
     */
    bool init(const device_config_t &device_config, const user_config_t &user_config, const std::string &zip_path);

    /**
     * @brief Deinitializes the audio manager.
     */
    void deinit();

    /**
     * @brief Checks if the onboard UCM config loaded and its mixer is open.
     *
     * @return true if available, false otherwise.
     */
    bool is_available() const {
        return cfg_loaded_ && mixer_.is_open();
    }

    /**
     * @brief Starts playback of the configured music file.
     *
     * Picks a USB DAC if one is connected, otherwise routes through the
     * onboard codec via UCM, following the current jack state.
     *
     * @return true if playback started (or was already running), false otherwise.
     */
    bool start_playback();

    /**
     * @brief Stops playback and waits for the playback thread to exit.
     */
    void stop_playback();

    /**
     * @brief Checks if a file is currently playing.
     *
     * @return true if playing, false otherwise.
     */
    bool is_playing() const {
        return playing_.load(std::memory_order_acquire);
    }

    /**
     * @brief Starts playback if idle, stops it if running.
     *
     * @return New playback state.
     */
    bool toggle_playback();

    /**
     * @brief Detects the current onboard audio output from jack state.
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
    /// regardless of whether playback is running.
    audio_output detected_output_ = audio_output::unknown;

    /// What the mixer was last actually set to via ucm::apply()
    audio_output mixer_applied_output_ = audio_output::unknown;

    /// Fixed file this build plays; music_scan_path + "/music.flac". No scanning yet.
    std::string music_file_path_;

    std::atomic<bool> playing_{false};
    std::atomic<bool> should_stop_{false};
    std::thread playback_thread_;
    std::vector<int> cpu_affinity_;
    bool onboard_reopen_workaround_ = false;

    /// Caps the PCM format offered to hardware regardless of what ALSA
    /// reports supporting (see device_config_t::audio_max_bit_depth). 0
    /// means trust the device-reported capability as-is.
    unsigned int max_bit_depth_ = 0;

    // LVGL timer for jack polling only (lightweight)
    lv_timer_t *poll_timer_ = nullptr;

    /**
     * @brief Applies onboard audio routing to the target output via UCM.
     *
     * @param target Target audio output.
     * @return true if applied successfully, false otherwise.
     */
    bool apply_output(audio_output target);

    void on_input_event(const input_event_t &event);

    /**
     * @brief Decodes and writes music_file_path_ to ALSA until it ends or
     * should_stop_ is set. Runs entirely on playback_thread_.
     */
    void playback_thread_func();

    static void poll_timer_trampoline(lv_timer_t *timer);
};

} // namespace Leticia
