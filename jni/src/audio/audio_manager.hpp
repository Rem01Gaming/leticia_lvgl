#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include <lvgl.h>

#include "ModernAlsa.hpp"
#include "device_config.hpp"
#include "input_event.hpp"
#include "ucm.hpp"

namespace Leticia {

/**
 * @brief Which physical path audio is (or should be) routed through.
 *        unknown only occurs before the first jack read has happened.
 */
enum class audio_output {
    unknown,
    speaker,
    headphones,
};

/**
 * @brief Owns the UCM config, mixer, and a small self-contained 1kHz test
 *        tone generator, so the UCM routing this project built can
 *        actually be exercised against real hardware from the UI.
 *
 *        Automatically follows the headphone jack: on insert/remove, if a
 *        tone is currently playing it is live-rerouted (UCM's declared
 *        `conflicts` teardown fires via ucm::apply()'s previous_device
 *        parameter); if nothing is playing, only the tracked jack state
 *        updates, and the mixer is brought in sync the next time playback
 *        starts. See apply_output()'s doc comment for why two separate
 *        "what does the jack say" / "what did the mixer last get set to"
 *        states are tracked rather than one.
 *
 *        Owns its own input_event_monitor, independent of power_manager's.
 *        Two separate open() calls against the same evdev node are safe --
 *        the kernel gives each file descriptor its own independent event
 *        queue -- and this keeps the audio and power subsystems decoupled,
 *        matching how each subsystem in this codebase already owns its
 *        own resources rather than sharing them through a third party.
 */
class audio_manager final {
public:
    audio_manager() = default;
    ~audio_manager();

    audio_manager(const audio_manager &) = delete;
    audio_manager &operator=(const audio_manager &) = delete;

    /**
     * @brief Loads the UCM config (see device_config.hpp's
     *        load_alsa_ucm_config_text() for where it's actually read
     *        from) and opens the mixer for @p device_config.alsa_card.
     * @return true if a UCM config was found, parsed, and the mixer
     *         opened -- i.e. is_available() would return true.
     *         false is not fatal to the caller: the UI should simply
     *         show the test-tone control as unavailable rather than
     *         treat this as a startup failure, the same way power_manager
     *         degrades gracefully when no backlight is found.
     */
    bool init(const device_config_t &device_config, const std::string &zip_path);

    /**
     * @brief Stops any playing tone and releases the mixer/pcm/input
     *        resources. Safe to call multiple times.
     */
    void deinit();

    /**
     * @brief Whether a UCM config was loaded and the mixer is open, i.e.
     *        whether start_test_tone() has any chance of succeeding.
     */
    bool is_available() const { return cfg_loaded_ && mixer_.is_open(); }

    /**
     * @brief Starts the 1kHz test tone, routed to whichever output the
     *        headphone jack currently indicates. No-op (returns true) if
     *        already playing.
     * @return false if is_available() is false or the pcm device could
     *         not be opened/configured.
     */
    bool start_test_tone();

    /**
     * @brief Stops the test tone and closes the pcm device. Deliberately
     *        does NOT tear down the mixer routing itself (no `disable`
     *        pass on the currently-applied device) -- UCM's conflict
     *        model only defines what happens when switching TO a
     *        different device, not an explicit "off" device, and
     *        inventing one wasn't needed for what this button does.
     *        Mixer state is simply left as most recently applied; the
     *        next start_test_tone() (on this device or after a jack
     *        change) re-applies routing from scratch regardless.
     */
    void stop_test_tone();

    bool is_test_tone_playing() const { return tone_playing_; }

    /**
     * @brief Toggles start/stop. Convenience for a single UI button.
     * @return The new playing state.
     */
    bool toggle_test_tone();

    /**
     * @brief Best current guess of the physical output, from the
     *        headphone jack switch (see input_event_monitor). Falls back
     *        to speaker if this board has no jack-sense switch at all.
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

    /// What the mixer was last actually set to via ucm::apply(), which
    /// only happens when a tone starts or live-reroutes while playing.
    /// Deliberately distinct from detected_output_: if the jack changes
    /// while nothing is playing, detected_output_ moves immediately but
    /// mixer_applied_output_ must NOT, or the next apply_output() call
    /// would think the mixer is already at the new target (since it'd
    /// compare against a value that silently tracked the jack instead of
    /// reality) and skip the conflict teardown that's actually still owed.
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
     * @brief Runs ucm::apply() for the "HiFi" verb toward @p target,
     *        using mixer_applied_output_ (not detected_output_) as the
     *        previous_device so conflict teardown fires correctly even
     *        across a period where the jack changed but nothing applied.
     *        Updates mixer_applied_output_ on success.
     */
    bool apply_output(audio_output target);

    void on_input_event(const input_event_t &event);
    void fill_buffer();

    static void fill_timer_trampoline(lv_timer_t *timer);
    static void poll_timer_trampoline(lv_timer_t *timer);
};

} // namespace Leticia
