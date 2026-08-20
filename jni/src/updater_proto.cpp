#include "updater_proto.hpp"

#include <cstdarg>

namespace Leticia {

updater_proto::~updater_proto()
{
    close();
}

bool updater_proto::attach(int fd)
{
    if (fd < 0)
        return false;

    pipe_ = fdopen(fd, "w");
    if (pipe_ == nullptr)
        return false;

    setlinebuf(pipe_);
    return true;
}

void updater_proto::ui_print(const char *fmt, ...)
{
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

void updater_proto::set_progress(float fraction, float seconds)
{
    if (pipe_ == nullptr)
        return;

    fprintf(pipe_, "set_progress %f\n", static_cast<double>(fraction));
    (void)seconds;
}

void updater_proto::close()
{
    if (pipe_ != nullptr) {
        fclose(pipe_);
        pipe_ = nullptr;
    }
}

} // namespace Leticia
