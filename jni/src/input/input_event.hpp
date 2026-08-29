#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * @brief Wrapper for Linux evdev and power_supply input sources.
 */
namespace Leticia {

enum class input_event_type {
    power_button_press,
    power_button_release,
    volume_up_press,
    volume_up_release,
    volume_down_press,
    volume_down_release,
    headphone_insert,
    headphone_remove,
    usb_connected,
    usb_disconnected,
};

struct input_event_t {
    input_event_type type;
    uint64_t timestamp_ms = 0;
};

using input_event_cb_t = std::function<void(const input_event_t &)>;

/**
 * @brief Monitors input events from various sources.
 */
class input_event_monitor final {
public:
    input_event_monitor() = default;
    ~input_event_monitor();

    input_event_monitor(const input_event_monitor &) = delete;
    input_event_monitor &operator=(const input_event_monitor &) = delete;

    /**
     * @brief Opens and probes input sources.
     *
     * @return true if at least one source was found, false otherwise.
     */
    bool open();

    /**
     * @brief Closes all opened input sources.
     */
    void close();

    /**
     * @brief Sets the callback for input events.
     *
     * @param callback Function to call on input events.
     */
    void set_callback(input_event_cb_t callback);

    /**
     * @brief Polls all input sources for new events.
     */
    void poll();

    /**
     * @brief Checks if headphones are connected.
     *
     * @return true if connected, false otherwise.
     */
    bool is_headphone_connected() const {
        return headphone_connected_;
    }

    /**
     * @brief Checks if the headphone state is known.
     *
     * @return true if known, false otherwise.
     */
    bool headphone_state_known() const {
        return headphone_state_known_;
    }

private:
    struct key_source {
        int fd = -1;
        std::string path;
    };

    std::vector<key_source> key_sources_;
    std::string usb_online_path_;
    bool usb_connected_ = false;
    bool usb_state_known_ = false;
    bool headphone_connected_ = false;
    bool headphone_state_known_ = false;
    input_event_cb_t callback_;

    void dispatch(input_event_type type) const;
    void poll_key_source(int fd);
    void poll_usb();

    static bool node_reports_watched_bits(int fd);
    static bool find_usb_online_node(std::string &out_path);
};

} // namespace Leticia
