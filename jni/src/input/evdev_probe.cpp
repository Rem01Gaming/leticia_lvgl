#include "evdev_probe.hpp"

#include <linux/input.h>
#include <sys/ioctl.h>

namespace Leticia::evdev_probe {

namespace {

constexpr unsigned int kBitsPerLong = sizeof(unsigned long) * 8;

bool ev_type_supported(int fd, unsigned int ev_type) {
    unsigned long ev_bits[(EV_MAX / kBitsPerLong) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0)
        return false;

    return has_bit(ev_bits, ev_type);
}

} // namespace

bool has_bit(const unsigned long *bitmask, unsigned int bit) {
    return (bitmask[bit / kBitsPerLong] >> (bit % kBitsPerLong)) & 1;
}

bool has_key(int fd, unsigned int key_code) {
    if (!ev_type_supported(fd, EV_KEY))
        return false;

    unsigned long key_bits[(KEY_MAX / kBitsPerLong) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0)
        return false;

    return has_bit(key_bits, key_code);
}

bool has_switch(int fd, unsigned int switch_code) {
    if (!ev_type_supported(fd, EV_SW))
        return false;

    unsigned long sw_bits[(SW_MAX / kBitsPerLong) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof(sw_bits)), sw_bits) < 0)
        return false;

    return has_bit(sw_bits, switch_code);
}

bool has_abs(int fd, unsigned int abs_code) {
    if (!ev_type_supported(fd, EV_ABS))
        return false;

    unsigned long abs_bits[(ABS_MAX / kBitsPerLong) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0)
        return false;

    return has_bit(abs_bits, abs_code);
}

} // namespace Leticia::evdev_probe
