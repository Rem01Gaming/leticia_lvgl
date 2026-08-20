#pragma once

namespace Leticia {

/**
 * @brief Freezes the parent (recovery) process for the lifetime of this
 *        object so it stops fighting us for the framebuffer, and guarantees
 *        it is resumed again, even on a crash in this binary.
 */
class parent_mute final {
public:
    parent_mute() = default;
    ~parent_mute();

    parent_mute(const parent_mute &) = delete;
    parent_mute &operator=(const parent_mute &) = delete;

    /**
     * @brief Send SIGSTOP to the process that exec'd us (recovery itself).
     *        Freezes every thread in that process, including its own GUI
     *        redraw loop. Installs fatal signal handlers so a crash in this
     *        binary cannot leave recovery permanently frozen.
     */
    void freeze();

    /**
     * @brief Send SIGCONT to the previously frozen parent, if any. Safe to
     *        call multiple times, only acts once.
     */
    void resume();
};

} // namespace Leticia
