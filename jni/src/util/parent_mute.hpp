#pragma once

namespace Leticia {

/**
 * Freezes the parent process during execution.
 */
class parent_mute final {
public:
    parent_mute() = default;
    ~parent_mute();

    parent_mute(const parent_mute &) = delete;
    parent_mute &operator=(const parent_mute &) = delete;

    /**
     * Freezes the parent process.
     */
    void freeze();

    /**
     * Resumes the parent process.
     */
    void resume();

    /**
     * Resumes the parent process from a signal handler or a watchdog thread.
     *
     * This function only calls async-signal-safe functions. Call it before
     * _exit(), because _exit() skips the atexit handlers.
     */
    static void emergency_resume();
};

} // namespace Leticia
