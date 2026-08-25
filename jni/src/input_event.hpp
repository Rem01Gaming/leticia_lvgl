#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * @brief Standardized wrapper around Linux evdev and power_supply sysfs
 *        sources for the physical inputs a recovery UI cares about: power
 *        button, volume up/down, headphone jack, and USB cable presence.
 */
namespace Leticia {

/**
 * @brief Kind of physical input event delivered through input_event_monitor.
 */
enum class input_event_type {
    power_button,
    volume_up,
    volume_down,
    headphone_insert,
    headphone_remove,
    usb_connected,
    usb_disconnected,
};

/**
 * @brief A single decoded, timestamped input event.
 */
struct input_event_t {
    input_event_type type;
    uint64_t timestamp_ms = 0;
};

using input_event_cb_t = std::function<void(const input_event_t &)>;

/**
 * @brief Discovers and polls every input source above through a single
 *        callback, so callers never touch evdev bit masks or sysfs paths
 *        directly. Non-owning of the callback target; call poll()
 *        periodically (e.g. from an LVGL timer) since sources are opened
 *        O_NONBLOCK.
 */
class input_event_monitor final {
public:
    input_event_monitor() = default;
    ~input_event_monitor();

    input_event_monitor(const input_event_monitor &) = delete;
    input_event_monitor &operator=(const input_event_monitor &) = delete;

    /**
     * @brief Probe /dev/input for nodes reporting KEY_POWER, KEY_VOLUMEUP,
     *        KEY_VOLUMEDOWN or SW_HEADPHONE_INSERT, and /sys/class/power_supply
     *        for a USB node reporting "online". A single evdev node commonly
     *        reports several of these (e.g. a gpio-keys power+volume combo),
     *        so it is opened once and shared rather than re-probed per key.
     * @return true if at least one source (evdev or USB) was found.
     */
    bool open();

    /**
     * @brief Close every opened source. Safe to call multiple times.
     */
    void close();

    /**
     * @brief Set the callback invoked for every decoded event. Replaces any
     *        previously set callback.
     */
    void set_callback(input_event_cb_t callback);

    /**
     * @brief Drain pending evdev events and re-check the USB "online" state,
     *        dispatching the callback for each change. Cheap to call from a
     *        timer since all reads are non-blocking.
     */
    void poll();

    /**
     * @brief Current headphone jack state, valid as of open() plus any
     *        events poll() has drained since. Unlike the event callback,
     *        this reflects the jack's state at the moment open() ran (via
     *        EVIOCGSW), not just transitions observed afterward -- so it
     *        gives the right answer even if headphones were already
     *        plugged in before this process started and no insert event
     *        has fired since.
     * @return true if headphones are connected, false if not connected OR
     *         if no node reporting SW_HEADPHONE_INSERT was found at all;
     *         check headphone_state_known() to distinguish those cases.
     */
    bool is_headphone_connected() const { return headphone_connected_; }

    /**
     * @brief Whether a node reporting SW_HEADPHONE_INSERT was found and its
     *        state successfully read at open(). false means this device
     *        has no jack-sense switch at all (or it couldn't be read), and
     *        is_headphone_connected() should not be trusted.
     */
    bool headphone_state_known() const { return headphone_state_known_; }

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
