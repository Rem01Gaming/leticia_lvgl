#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Leticia::alsa_scan {

/**
 * Describes one playback PCM node found in /dev/snd.
 */
struct playback_pcm {
    unsigned int card = 0;
    unsigned int device = 0;
    bool usb = false;
};

/**
 * Lists the playback PCM nodes without opening them.
 *
 * Opening a PCM node fails with EBUSY while another thread plays on it.
 * This function only reads directory entries and sysfs files.
 *
 * @return Playback nodes sorted by card and device.
 */
std::vector<playback_pcm> list_playback();

/**
 * Finds the first USB playback PCM that is not the onboard card.
 *
 * @param onboard_card Card index of the onboard codec.
 * @param out_card Receives the card index.
 * @param out_device Receives the device index.
 * @return true if a USB playback PCM exists, false otherwise.
 */
bool find_usb_playback(unsigned int onboard_card, unsigned int &out_card, unsigned int &out_device);

/**
 * Builds a value that changes when a playback PCM node appears or disappears.
 *
 * @return Hash of the current playback node set.
 */
uint64_t playback_signature();

/**
 * Reads a short human readable name of a sound card.
 *
 * @param card Card index.
 * @return USB product name, card id, or "card N" if nothing is readable.
 */
std::string card_label(unsigned int card);

} // namespace Leticia::alsa_scan
