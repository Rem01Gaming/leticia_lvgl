#pragma once

#include <cstdint>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace Leticia {

/**
 * @brief Owns a libswresample context for one input/output format pair.
 */
class resampler final {
public:
    resampler() = default;
    ~resampler();

    resampler(const resampler &) = delete;
    resampler &operator=(const resampler &) = delete;

    /**
     * @brief Configures the resampler for a given input/output format pair.
     *
     * Replaces any context from a previous call.
     *
     * @param in_rate Input sample rate in Hz.
     * @param in_layout Input channel layout.
     * @param in_fmt Input sample format.
     * @param out_rate Output sample rate in Hz.
     * @param out_layout Output channel layout.
     * @param out_fmt Output sample format.
     * @return true if the context was created successfully, false otherwise.
     */
    bool init(int in_rate, AVChannelLayout in_layout, AVSampleFormat in_fmt, int out_rate,
              AVChannelLayout out_layout, AVSampleFormat out_fmt);

    /**
     * @brief Converts one block of input samples.
     *
     * @param in_data Input planes, in the layout swresample expects for in_fmt.
     * @param in_samples Number of input samples per channel.
     * @param out_data Output buffer; must hold at least max_out_samples samples per channel.
     * @param max_out_samples Capacity of out_data, in samples per channel.
     * @return Number of output samples per channel, or a negative value on error.
     */
    int convert(const uint8_t **in_data, int in_samples, uint8_t **out_data, int max_out_samples);

    /**
     * @brief Returns an upper bound of the output samples for one input block.
     *
     * The bound includes the samples that the resampler still buffers.
     *
     * @param in_samples Number of input samples per channel.
     * @return Output sample count per channel, or 0 if no context is set up.
     */
    int max_output_samples(int in_samples) const;

    /**
     * @brief Writes out the samples that the resampler still buffers.
     *
     * @param out_data Output buffer; must hold at least max_out_samples samples per channel.
     * @param max_out_samples Capacity of out_data, in samples per channel.
     * @return Number of output samples per channel, or a negative value on error.
     */
    int flush(uint8_t **out_data, int max_out_samples);

private:
    SwrContext *swr_ = nullptr;
};

} // namespace Leticia
