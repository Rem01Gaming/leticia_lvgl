#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
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

struct music_track_t {
    std::string path;
    std::string title;
    std::string artist;
    uint64_t duration_seconds = 0;

    /** Cover pixels in RGB565 little endian. Empty if the track has no cover. */
    std::vector<uint8_t> cover_pixels;
    unsigned int cover_width = 0;
    unsigned int cover_height = 0;
};

struct audio_device_info_t {
    std::string name = "Unavailable";
    std::string output = "Speaker";
    std::string format = "-";
    std::string supported_formats = "-";
    std::string supported_rates = "-";
    unsigned int card = 0;
    unsigned int device = 0;
    unsigned int sample_rate = 0;
    unsigned int max_channels = 0;
    size_t channels = 0;
    bool active = false;
};

/**
 * Enumeration of onboard audio output paths.
 *
 * A USB DAC is addressed by card and device and never goes through this enum.
 */
enum class audio_output {
    unknown,    /**< Unknown state. */
    speaker,    /**< Speaker output. */
    headphones, /**< Headphone or AUX output. */
};

/**
 * Manages the music library, audio routing and playback.
 *
 * Output priority is USB DAC, then the headphone jack, then the speaker.
 * Every public function must run on the LVGL thread.
 */
class audio_manager final {
public:
    audio_manager() = default;
    ~audio_manager();

    audio_manager(const audio_manager &) = delete;
    audio_manager &operator=(const audio_manager &) = delete;

    /**
     * Initializes routing and starts the background library scan.
     *
     * @param device_config Device configuration.
     * @param user_config User configuration. The scan path comes from here.
     * @param zip_path Path to the OTA zip file.
     * @param cover_size_px Edge length of the decoded cover images, in pixels.
     * @return true if the onboard UCM config loaded, false otherwise. A USB DAC still works.
     */
    bool init(const device_config_t &device_config, const user_config_t &user_config, const std::string &zip_path,
              unsigned int cover_size_px);

    /**
     * Stops playback and releases every resource.
     */
    void deinit();

    /**
     * Checks if the onboard UCM config loaded and its mixer is open.
     *
     * @return true if available, false otherwise.
     */
    bool is_available() const {
        return cfg_loaded_ && mixer_.is_open();
    }

    /**
     * Moves the finished scan result into the track list.
     *
     * @return true once, when the scan has just finished. false otherwise.
     */
    bool take_scan_result();

    /**
     * Gets the number of files the scan has processed so far.
     *
     * @return File count.
     */
    size_t scan_progress() const { return scan_progress_.load(std::memory_order_relaxed); }

    /**
     * Starts or resumes playback of the current track.
     *
     * @return true if playback started or was already running, false otherwise.
     */
    bool start_playback();

    /**
     * Stops playback and waits for the playback thread to exit.
     */
    void stop_playback();

    bool is_playing() const { return playing_.load(std::memory_order_acquire); }
    bool is_paused() const {
        return paused_.load(std::memory_order_acquire) || auto_paused_.load(std::memory_order_acquire);
    }

    /**
     * Checks if audio is audible right now.
     *
     * @return true if a session runs and is not paused.
     */
    bool is_audible() const { return is_playing() && !is_paused(); }

    uint64_t position_seconds() const { return position_seconds_.load(std::memory_order_acquire); }
    bool take_track_finished() { return track_finished_.exchange(false, std::memory_order_acq_rel); }

    /**
     * Gets a copy of the current output device description.
     *
     * @return Device information.
     */
    audio_device_info_t device_info() const;

    /**
     * Plays a track from the start.
     *
     * @param index Track index.
     * @return true if playback started, false otherwise.
     */
    bool play_track(size_t index);

    /**
     * Seeks inside the current track. Works while paused or stopped.
     *
     * @param seconds Target position in seconds.
     */
    void seek_to(uint64_t seconds);

    /**
     * Pauses or resumes. Starts playback if idle.
     *
     * @return true if audio is audible after the call.
     */
    bool toggle_playback();

    /**
     * Gets the playback volume.
     *
     * @return Volume in percent, 0 to 100.
     */
    int volume() const { return volume_percent_.load(std::memory_order_acquire); }

    /**
     * Sets the playback volume. The change is applied to the running session within one audio buffer.
     *
     * @param percent Volume in percent. Values outside 0 to 100 are clamped.
     */
    void set_volume(int percent);

    /**
     * Changes the playback volume by a relative amount.
     *
     * @param delta_percent Change in percent. Negative values lower the volume.
     */
    void adjust_volume(int delta_percent);

    /**
     * Checks if the volume was set since the last call. Used to show the volume indicator.
     *
     * @return true once after each change, even if the value hit a limit.
     */
    bool take_volume_changed() { return volume_changed_.exchange(false, std::memory_order_acq_rel); }

    const std::vector<music_track_t> &tracks() const { return tracks_; }
    size_t current_track_index() const { return current_track_index_; }
    const music_track_t *current_track() const;

    /**
     * Detects the onboard output from the jack state.
     *
     * @return Detected audio output.
     */
    audio_output detected_output() const;

private:
    struct output_target {
        ModernAlsa::size_type card = 0;
        ModernAlsa::size_type device = 0;
        size_t channels = 2;
        bool via_usb = false;
    };

    ucm::config cfg_;
    bool cfg_loaded_ = false;
    ModernAlsa::mixer mixer_;
    unsigned int card_ = 0;
    input_event_monitor input_monitor_;
    audio_output detected_output_ = audio_output::unknown;
    audio_output mixer_applied_output_ = audio_output::unknown;

    std::vector<music_track_t> tracks_;
    std::vector<music_track_t> scanned_;
    std::thread scan_thread_;
    std::atomic<bool> scan_done_{false};
    std::atomic<bool> scan_abort_{false};
    std::atomic<size_t> scan_progress_{0};
    std::string scan_path_;
    unsigned int cover_size_px_ = 64;

    size_t current_track_index_ = 0;
    std::string music_file_path_;

    std::atomic<bool> playing_{false};
    std::atomic<bool> should_stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> auto_paused_{false};
    std::atomic<bool> track_finished_{false};
    std::atomic<uint64_t> position_seconds_{0};
    std::atomic<int64_t> seek_request_seconds_{-1};
    std::atomic<bool> resume_pending_{false};
    std::atomic<bool> via_usb_active_{false};
    std::atomic<int> volume_percent_{100};
    std::atomic<bool> volume_changed_{false};
    uint32_t last_device_check_ms_ = 0;
    bool volume_up_held_ = false;
    bool volume_down_held_ = false;
    int volume_repeat_direction_ = 0;
    uint32_t volume_next_repeat_ms_ = 0;
    bool usb_present_ = false;
    uint64_t pcm_signature_ = 0;

    mutable std::mutex device_info_mutex_;
    audio_device_info_t device_info_;

    std::thread playback_thread_;
    std::vector<int> cpu_affinity_;
    bool onboard_reopen_workaround_ = false;
    unsigned int max_bit_depth_ = 0;
    lv_timer_t *poll_timer_ = nullptr;

    /**
     * Routes the onboard mixer to the target output through UCM.
     *
     * @param target Target audio output.
     * @return true if applied, false otherwise.
     */
    bool apply_output(audio_output target);

    /**
     * Disables the mixer directives of the applied onboard device.
     */
    void mute_output();

    /**
     * Stops playback and starts it again so the output route is resolved again.
     */
    void restart_playback();

    /**
     * Stops playback and keeps the position. The next play resumes on the current route.
     */
    void pause_for_route_loss();

    /**
     * Starts or stops the volume key auto-repeat and applies the first step on a press.
     *
     * @param up true for the volume up key, false for volume down.
     * @param pressed true on press, false on release.
     */
    void on_volume_key(bool up, bool pressed);

    /**
     * Applies repeated volume steps while a volume key is held.
     *
     * @param now_ms Current LVGL tick in milliseconds.
     */
    void service_volume_repeat(uint32_t now_ms);

    /**
     * Reacts to a jack event.
     *
     * @param event Input event.
     */
    void on_input_event(const input_event_t &event);

    /**
     * Reacts to a USB DAC that appeared or disappeared.
     *
     * @param present true if a USB playback device exists now.
     */
    void on_usb_changed(bool present);

    /**
     * Updates the device information shown while no session runs.
     */
    void refresh_idle_device_info();

    /**
     * Scans the music path. Runs on scan_thread_.
     */
    void scan_worker();

    /**
     * Decodes and plays music_file_path_ until it ends or should_stop_ is set.
     * Runs on playback_thread_.
     */
    void playback_thread_func();

    static void poll_timer_trampoline(lv_timer_t *timer);
};

} // namespace Leticia
