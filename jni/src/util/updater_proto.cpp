#include "updater_proto.hpp"

#include <cstdarg>

namespace Leticia {

updater_proto::~updater_proto() {
    close();
}

bool updater_proto::attach(int fd) {
    if (fd < 0)
        return false;

    pipe_ = fdopen(fd, "w");
    if (pipe_ == nullptr)
        return false;

    setlinebuf(pipe_);
    return true;
}

void updater_proto::close() {
    if (pipe_ != nullptr) {
        fclose(pipe_);
        pipe_ = nullptr;
    }
}

} // namespace Leticia

namespace Leticia {
static updater_proto *g_updater_proto = nullptr;

void set_updater_proto(updater_proto *p) {
    g_updater_proto = p;
}

void clear_updater_proto() {
    g_updater_proto = nullptr;
}

void ui_print(const char *fmt, ...) {
    if (g_updater_proto != nullptr && g_updater_proto->get_pipe() != nullptr) {
        char msg[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);
        fprintf(g_updater_proto->get_pipe(), "ui_print %s\n", msg);
        fprintf(g_updater_proto->get_pipe(), "ui_print\n");
        return;
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

void ui_error(const char *fmt, ...) {
    if (g_updater_proto != nullptr && g_updater_proto->get_pipe() != nullptr) {
        char msg[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);
        fprintf(g_updater_proto->get_pipe(), "ui_print_color error %s\n", msg);
        fprintf(g_updater_proto->get_pipe(), "ui_print\n");
        return;
    }

    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "error: ");
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

void ui_warning(const char *fmt, ...) {
    if (g_updater_proto != nullptr && g_updater_proto->get_pipe() != nullptr) {
        char msg[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);
        fprintf(g_updater_proto->get_pipe(), "ui_print_color warning %s\n", msg);
        fprintf(g_updater_proto->get_pipe(), "ui_print\n");
        return;
    }

    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "warning: ");
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

void ui_set_progress(float fraction, float seconds) {
    if (g_updater_proto != nullptr && g_updater_proto->get_pipe() != nullptr) {
        fprintf(g_updater_proto->get_pipe(), "set_progress %f\n", static_cast<double>(fraction));
        (void)seconds;
    }
}

} // namespace Leticia
