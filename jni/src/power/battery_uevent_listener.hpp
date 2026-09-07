#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace Leticia {

/**
 * @brief Listens for kernel power_supply uevents over a netlink socket.
 *
 * Requires root: NETLINK_KOBJECT_UEVENT sockets are only bindable by
 * privileged processes on stock Android/Linux kernels.
 *
 * Runs its own background thread. The callback fires on that thread,
 * not the caller's thread; the caller is responsible for marshalling
 * to whatever thread it needs (e.g. lv_async_call for LVGL).
 */
class battery_uevent_listener final {
public:
    battery_uevent_listener() = default;
    ~battery_uevent_listener();

    battery_uevent_listener(const battery_uevent_listener &) = delete;
    battery_uevent_listener &operator=(const battery_uevent_listener &) = delete;

    /**
     * @brief Opens the netlink socket and starts the listener thread.
     *
     * @param node_name Basename of the power_supply node to filter on,
     *        e.g. "battery" for /sys/class/power_supply/battery.
     * @param on_event Callback invoked when a matching uevent arrives.
     * @return true if the socket was opened and the thread started.
     */
    bool start(std::string node_name, std::function<void()> on_event);

    /**
     * @brief Stops the listener thread and closes the socket.
     */
    void stop();

    /**
     * @brief Checks whether the listener is currently running.
     *
     * @return true if active, false otherwise.
     */
    bool is_running() const { return running_.load(); }

private:
    int sock_fd_ = -1;
    int wake_fd_ = -1;
    std::string node_name_;
    std::function<void()> on_event_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    void run();
    bool message_matches(const char *buf, size_t len) const;
};

} // namespace Leticia
