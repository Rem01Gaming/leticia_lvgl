#include "alsa_scan.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <unistd.h>

namespace Leticia::alsa_scan {

namespace {

constexpr const char *kSndDir = "/dev/snd";

/**
 * Reads the first line of a small text file.
 *
 * @param path File path.
 * @return Line without trailing whitespace, or an empty string on failure.
 */
std::string read_first_line(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == nullptr)
        return {};

    char buf[128] = {0};
    const char *got = fgets(buf, sizeof(buf), f);
    fclose(f);
    if (got == nullptr)
        return {};

    size_t len = strlen(buf);
    while (len > 0 && isspace(static_cast<unsigned char>(buf[len - 1])))
        buf[--len] = '\0';
    return std::string(buf);
}

/**
 * Checks whether a sound card sits on the USB bus.
 *
 * @param card Card index.
 * @return true if the card is a USB audio device.
 */
bool card_is_usb(unsigned int card) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/proc/asound/card%u/usbid", card);
    if (access(path, F_OK) == 0)
        return true;

    char link[PATH_MAX];
    char resolved[PATH_MAX];
    snprintf(link, sizeof(link), "/sys/class/sound/card%u/device", card);
    if (realpath(link, resolved) == nullptr)
        return false;

    std::string lowered(resolved);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered.find("/usb") != std::string::npos;
}

} // namespace

std::vector<playback_pcm> list_playback() {
    std::vector<playback_pcm> result;

    DIR *dir = opendir(kSndDir);
    if (dir == nullptr)
        return result;

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        unsigned int card = 0;
        unsigned int device = 0;
        char kind = 0;
        if (sscanf(entry->d_name, "pcmC%uD%u%c", &card, &device, &kind) != 3 || kind != 'p')
            continue;

        playback_pcm pcm;
        pcm.card = card;
        pcm.device = device;
        pcm.usb = card_is_usb(card);
        result.push_back(pcm);
    }
    closedir(dir);

    std::sort(result.begin(), result.end(), [](const playback_pcm &a, const playback_pcm &b) {
        return a.card != b.card ? a.card < b.card : a.device < b.device;
    });
    return result;
}

bool find_usb_playback(unsigned int onboard_card, unsigned int &out_card, unsigned int &out_device) {
    for (const playback_pcm &pcm : list_playback()) {
        if (pcm.card == onboard_card || !pcm.usb)
            continue;

        out_card = pcm.card;
        out_device = pcm.device;
        return true;
    }
    return false;
}

uint64_t playback_signature() {
    uint64_t hash = 1469598103934665603ULL;
    for (const playback_pcm &pcm : list_playback()) {
        hash = (hash ^ (static_cast<uint64_t>(pcm.card) << 8 | pcm.device)) * 1099511628211ULL;
        hash = (hash ^ (pcm.usb ? 1U : 0U)) * 1099511628211ULL;
    }
    return hash;
}

std::string card_label(unsigned int card) {
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "/sys/class/sound/card%u/device/../product", card);
    std::string label = read_first_line(path);
    if (!label.empty())
        return label;

    snprintf(path, sizeof(path), "/proc/asound/card%u/id", card);
    label = read_first_line(path);
    if (!label.empty())
        return label;

    snprintf(path, sizeof(path), "card %u", card);
    return std::string(path);
}

} // namespace Leticia::alsa_scan
