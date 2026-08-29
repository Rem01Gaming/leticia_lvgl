#include "touch_probe.hpp"
#include "evdev_probe.hpp"

#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>

namespace Leticia::touch_probe {

namespace {

bool node_is_touchscreen(int fd) {
    return evdev_probe::has_abs(fd, ABS_MT_POSITION_X) || evdev_probe::has_abs(fd, ABS_X);
}

} // namespace

std::optional<std::string> find() {
    DIR *dir = opendir("/dev/input");
    if (dir == nullptr)
        return std::nullopt;

    std::optional<std::string> found;
    struct dirent *entry;

    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0)
            continue;

        std::string path = std::string("/dev/input/") + entry->d_name;

        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;

        if (node_is_touchscreen(fd))
            found = path;

        close(fd);

        if (found)
            break;
    }

    closedir(dir);
    return found;
}

} // namespace Leticia::touch_probe
