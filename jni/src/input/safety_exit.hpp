#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace Leticia {

/**
 * Watchdog that ends the process when the user holds the power button.
 *
 * The watchdog runs on its own thread with its own evdev descriptors. It does
 * not depend on the LVGL main loop. The user can leave the UI even if the main
 * thread hangs. The volume up and volume down keys held together have the same effect.
 */
class safety_exit_monitor final {
public:
    safety_exit_monitor() = default;
    ~safety_exit_monitor();

    safety_exit_monitor(const safety_exit_monitor &) = delete;
    safety_exit_monitor &operator=(const safety_exit_monitor &) = delete;

    /**
     * Starts the watchdog thread.
     *
     * @param on_long_press Called once from the watchdog thread after the hold time.
     *        The callback must request a normal shutdown, for example set an exit flag.
     * @param on_force_exit Called from the watchdog thread just before the forced exit.
     *        The callback must not block. It can be empty.
     */
    void start(std::function<void()> on_long_press, std::function<void()> on_force_exit);

    /**
     * Arms the forced exit deadline for a shutdown that already started.
     *
     * The watchdog terminates the process if shutdown takes longer than the timeout.
     *
     * @param timeout_ms Time allowed for the shutdown, in milliseconds.
     */
    void begin_shutdown(uint32_t timeout_ms);

    /**
     * Stops the watchdog thread and closes its input nodes.
     */
    void stop();

private:
    struct key_source {
        int fd = -1;
    };

    /**
     * Opens every input node that reports the power key or both volume keys.
     */
    void open_key_sources();

    /**
     * Closes all input nodes.
     */
    void close_key_sources();

    /**
     * Reads pending events from one node and updates the held key state.
     *
     * @param index Index of the node in key_sources_.
     * @return false if the node failed and was closed, true otherwise.
     */
    bool drain_source(size_t index);

    /**
     * Watchdog thread entry point.
     */
    void run();

    std::vector<key_source> key_sources_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> deadline_ms_{0};
    std::function<void()> on_long_press_;
    std::function<void()> on_force_exit_;
    bool power_held_ = false;
    bool volume_up_held_ = false;
    bool volume_down_held_ = false;
};

} // namespace Leticia
