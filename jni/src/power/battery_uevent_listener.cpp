#include "power/battery_uevent_listener.hpp"
#include "util/updater_proto.hpp"

#include <cstring>
#include <linux/netlink.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace Leticia {

namespace {
constexpr int kNetlinkKobjectUevent = 15; // NETLINK_KOBJECT_UEVENT, no kernel group id needed for kobj group 1
constexpr uint32_t kKobjectUeventGroup = 1;
constexpr size_t kRecvBufSize = 2048;
} // namespace

battery_uevent_listener::~battery_uevent_listener() {
    stop();
}

bool battery_uevent_listener::start(std::string node_name, std::function<void()> on_event) {
    if (running_.load())
        return false;

    sock_fd_ = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, kNetlinkKobjectUevent);
    if (sock_fd_ < 0) {
        Leticia::ui_print("battery_uevent_listener: socket() failed, errno=%d", errno);
        return false;
    }

    sockaddr_nl addr{};
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = 0; // let the kernel assign
    addr.nl_groups = kKobjectUeventGroup;

    if (bind(sock_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        Leticia::ui_print("battery_uevent_listener: bind() failed, errno=%d (need root?)", errno);
        close(sock_fd_);
        sock_fd_ = -1;
        return false;
    }

    wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd_ < 0) {
        Leticia::ui_print("battery_uevent_listener: eventfd() failed, errno=%d", errno);
        close(sock_fd_);
        sock_fd_ = -1;
        return false;
    }

    node_name_ = std::move(node_name);
    on_event_ = std::move(on_event);
    running_.store(true);
    thread_ = std::thread(&battery_uevent_listener::run, this);
    return true;
}

void battery_uevent_listener::stop() {
    if (!running_.load())
        return;

    running_.store(false);

    if (wake_fd_ >= 0) {
        uint64_t one = 1;
        ssize_t written = write(wake_fd_, &one, sizeof(one));
        (void)written; // best effort wake, thread also checks running_ on timeout
    }

    if (thread_.joinable())
        thread_.join();

    if (sock_fd_ >= 0) {
        close(sock_fd_);
        sock_fd_ = -1;
    }
    if (wake_fd_ >= 0) {
        close(wake_fd_);
        wake_fd_ = -1;
    }
}

bool battery_uevent_listener::message_matches(const char *buf, size_t len) const {
    // Uevent payload is a sequence of NUL-separated "KEY=VALUE" strings.
    // We only care that SUBSYSTEM=power_supply and the path contains our node name.
    bool is_power_supply = false;
    bool matches_node = node_name_.empty();

    size_t offset = 0;
    while (offset < len) {
        const char *field = buf + offset;
        size_t field_len = strnlen(field, len - offset);

        if (strncmp(field, "SUBSYSTEM=power_supply", field_len) == 0)
            is_power_supply = true;
        else if (!matches_node && strncmp(field, "POWER_SUPPLY_NAME=", 18) == 0 &&
                 std::string(field + 18, field_len - 18) == node_name_)
            matches_node = true;

        offset += field_len + 1;
    }

    return is_power_supply && matches_node;
}

void battery_uevent_listener::run() {
    char buf[kRecvBufSize];

    pollfd fds[2];
    fds[0].fd = sock_fd_;
    fds[0].events = POLLIN;
    fds[1].fd = wake_fd_;
    fds[1].events = POLLIN;

    while (running_.load()) {
        fds[0].revents = 0;
        fds[1].revents = 0;

        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            Leticia::ui_print("battery_uevent_listener: poll() failed, errno=%d", errno);
            break;
        }

        if (fds[1].revents & POLLIN)
            break; // stop() requested

        if (fds[0].revents & POLLIN) {
            ssize_t n = recv(sock_fd_, buf, sizeof(buf) - 1, 0);
            if (n <= 0)
                continue;
            buf[n] = '\0';

            if (message_matches(buf, static_cast<size_t>(n)) && on_event_)
                on_event_();
        }
    }
}

} // namespace Leticia
