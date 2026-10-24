#include "updater_proto.hpp"

#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace Leticia {

namespace {

constexpr int kMessageCapacity = 512;
constexpr int kPacketCapacity = kMessageCapacity + 32;

updater_proto *g_updater_proto = nullptr;

enum class print_kind {
    plain,
    error,
    warning,
};

/**
 * Replaces line breaks so a message cannot inject extra protocol lines.
 *
 * @param text Message text. Changed in place.
 */
void flatten_line_breaks(char *text) {
    for (char *c = text; *c != '\0'; ++c) {
        if (*c == '\n' || *c == '\r')
            *c = ' ';
    }
}

/**
 * Formats one print message and writes it to the pipe or to stderr.
 *
 * @param kind Message category.
 * @param fmt Printf style format string.
 * @param args Format arguments.
 */
void emit(print_kind kind, const char *fmt, va_list args) {
    char msg[kMessageCapacity];
    vsnprintf(msg, sizeof(msg), fmt, args);
    flatten_line_breaks(msg);

    if (g_updater_proto == nullptr) {
        const char *prefix = kind == print_kind::error ? "error: " : kind == print_kind::warning ? "warning: " : "";
        fprintf(stderr, "%s%s\n", prefix, msg);
        return;
    }

    const char *command = kind == print_kind::error     ? "ui_print_color error "
                          : kind == print_kind::warning ? "ui_print_color warning "
                                                        : "ui_print ";
    char packet[kPacketCapacity];
    int size = snprintf(packet, sizeof(packet), "%s%s\nui_print\n", command, msg);
    if (size > 0 && size < static_cast<int>(sizeof(packet)))
        g_updater_proto->write_message(packet, size);
}

} // namespace

updater_proto::~updater_proto() {
    close();
}

bool updater_proto::attach(int fd) {
    if (fd < 0)
        return false;

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return false;

    signal(SIGPIPE, SIG_IGN);
    fd_ = fd;
    return true;
}

void updater_proto::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void updater_proto::write_message(const char *data, int size) const {
    if (fd_ < 0 || size <= 0)
        return;

    ssize_t written;
    do {
        written = write(fd_, data, static_cast<size_t>(size));
    } while (written < 0 && errno == EINTR);
}

void set_updater_proto(updater_proto *p) {
    g_updater_proto = p;
}

void clear_updater_proto() {
    g_updater_proto = nullptr;
}

void ui_print(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit(print_kind::plain, fmt, args);
    va_end(args);
}

void ui_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit(print_kind::error, fmt, args);
    va_end(args);
}

void ui_warning(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit(print_kind::warning, fmt, args);
    va_end(args);
}

void ui_set_progress(float fraction, float seconds) {
    (void)seconds;
    if (g_updater_proto == nullptr)
        return;

    char packet[64];
    int size = snprintf(packet, sizeof(packet), "set_progress %f\n", static_cast<double>(fraction));
    if (size > 0 && size < static_cast<int>(sizeof(packet)))
        g_updater_proto->write_message(packet, size);
}

} // namespace Leticia
