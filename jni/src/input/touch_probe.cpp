#include "touch_probe.hpp"

#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace Leticia::touch_probe {

namespace {

bool has_bit(const unsigned long *bitmask, int bit) {
    constexpr int bits_per_long = sizeof(unsigned long) * 8;
    return (bitmask[bit / bits_per_long] >> (bit % bits_per_long)) & 1;
}

bool node_is_touchscreen(int fd) {
    unsigned long ev_bits[(EV_MAX / (sizeof(unsigned long) * 8)) + 1] = {0};
    unsigned long abs_bits[(ABS_MAX / (sizeof(unsigned long) * 8)) + 1] = {0};

    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0)
        return false;

    if (!has_bit(ev_bits, EV_ABS))
        return false;

    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0)
        return false;

    return has_bit(abs_bits, ABS_MT_POSITION_X) || has_bit(abs_bits, ABS_X);
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
