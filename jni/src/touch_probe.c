#include "touch_probe.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BITS_TO_LONGS(n) (((n) + (sizeof(long) * 8) - 1) / (sizeof(long) * 8))

static int has_bit(const unsigned long *bitmask, int bit)
{
    return (bitmask[bit / (sizeof(long) * 8)] >> (bit % (sizeof(long) * 8))) & 1;
}

static int node_is_touchscreen(int fd)
{
    unsigned long ev_bits[BITS_TO_LONGS(EV_MAX)] = {0};
    unsigned long abs_bits[BITS_TO_LONGS(ABS_MAX)] = {0};

    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0)
        return 0;

    if (!has_bit(ev_bits, EV_ABS))
        return 0;

    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0)
        return 0;

    return has_bit(abs_bits, ABS_MT_POSITION_X) || has_bit(abs_bits, ABS_X);
}

int touch_probe_find(char *out_path, size_t out_size)
{
    DIR *dir = opendir("/dev/input");
    if (dir == NULL)
        return -1;

    struct dirent *entry;
    int found = -1;

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0)
            continue;

        char path[TOUCH_PROBE_MAX_PATH];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;

        if (node_is_touchscreen(fd)) {
            snprintf(out_path, out_size, "%s", path);
            found = 0;
            close(fd);
            break;
        }

        close(fd);
    }

    closedir(dir);
    return found;
}
