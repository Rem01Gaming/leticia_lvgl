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

void updater_proto::ui_print(const char *fmt, ...) {
    if (pipe_ == nullptr)
        return;

    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    fprintf(pipe_, "ui_print %s\n", msg);
    fprintf(pipe_, "ui_print\n");
}

void updater_proto::set_progress(float fraction, float seconds) {
    if (pipe_ == nullptr)
        return;

    fprintf(pipe_, "set_progress %f\n", static_cast<double>(fraction));
    (void)seconds;
}

void updater_proto::close() {
    if (pipe_ != nullptr) {
        fclose(pipe_);
        pipe_ = nullptr;
    }
}

} // namespace Leticia

// Global bridge implementation
namespace Leticia {
static updater_proto *g_updater_proto = nullptr;

void set_updater_proto(updater_proto *p) {
    g_updater_proto = p;
}

void clear_updater_proto() {
    g_updater_proto = nullptr;
}

void ui_print(const char *fmt, ...) {
    if (g_updater_proto != nullptr) {
        char msg[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);
        g_updater_proto->ui_print("%s", msg);
        return;
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

} // namespace Leticia
