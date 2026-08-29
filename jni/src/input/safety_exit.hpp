#pragma once

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

namespace Leticia {

/**
 * @brief Watchdog that force-exits the process on a long press of the power button.
 *
 * Runs on its own thread with its own evdev file descriptor, independent of the
 * LVGL main loop. This gives the user a way to close the UI even if the main
 * thread has hung and the normal exit button or power-button toggle no longer
 * responds.
 */
class safety_exit_monitor final {
public:
    safety_exit_monitor() = default;
    ~safety_exit_monitor();

    safety_exit_monitor(const safety_exit_monitor &) = delete;
    safety_exit_monitor &operator=(const safety_exit_monitor &) = delete;

    /**
     * @brief Opens the power button input node and starts the watchdog thread.
     *
     * @param on_long_press Called once from the watchdog thread after the power button
     *        has been held continuously for the trigger duration. The callback should
     *        request a normal shutdown (e.g. set an exit flag). If the process has
     *        not exited shortly afterwards, the watchdog forcibly terminates it.
     */
    void start(std::function<void()> on_long_press);

    /**
     * @brief Stops the watchdog thread and closes its input nodes.
     */
    void stop();

private:
    struct key_source {
        int fd = -1;
    };

    void open_key_sources();
    void close_key_sources();
    void run();

    std::vector<key_source> key_sources_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::function<void()> on_long_press_;
};

} // namespace Leticia
