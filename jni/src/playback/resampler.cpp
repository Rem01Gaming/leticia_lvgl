#include "resampler.hpp"

#include <vector>

extern "C" {
#include <libavutil/opt.h>
}

namespace Leticia {

resampler::~resampler() {
    if (swr_ != nullptr) swr_free(&swr_);
}

bool resampler::init(int in_rate, AVChannelLayout in_layout, AVSampleFormat in_fmt, int out_rate,
                     AVChannelLayout out_layout, AVSampleFormat out_fmt) {
    if (swr_ != nullptr) swr_free(&swr_);

    swr_ = swr_alloc();
    if (swr_ == nullptr) return false;

    int status = av_opt_set_chlayout(swr_, "in_chlayout", &in_layout, 0);
    status |= av_opt_set_int(swr_, "in_sample_rate", in_rate, 0);
    status |= av_opt_set_sample_fmt(swr_, "in_sample_fmt", in_fmt, 0);
    status |= av_opt_set_chlayout(swr_, "out_chlayout", &out_layout, 0);
    status |= av_opt_set_int(swr_, "out_sample_rate", out_rate, 0);
    status |= av_opt_set_sample_fmt(swr_, "out_sample_fmt", out_fmt, 0);
    if (status < 0) {
        swr_free(&swr_);
        return false;
    }

    if (out_layout.nb_channels == 1 && in_layout.nb_channels > 1) {
        std::vector<double> matrix(static_cast<size_t>(in_layout.nb_channels), 1.0 / in_layout.nb_channels);
        swr_set_matrix(swr_, matrix.data(), in_layout.nb_channels);
    }

    if (swr_init(swr_) < 0) {
        swr_free(&swr_);
        return false;
    }
    return true;
}

int resampler::convert(const uint8_t **in_data, int in_samples, uint8_t **out_data, int max_out_samples) {
    if (swr_ == nullptr) return -1;
    return swr_convert(swr_, out_data, max_out_samples, in_data, in_samples);
}

int resampler::max_output_samples(int in_samples) const {
    if (swr_ == nullptr) return 0;
    const int bound = swr_get_out_samples(swr_, in_samples);
    return bound > 0 ? bound + 32 : in_samples + 32;
}

int resampler::flush(uint8_t **out_data, int max_out_samples) {
    if (swr_ == nullptr) return -1;
    return swr_convert(swr_, out_data, max_out_samples, nullptr, 0);
}

} // namespace Leticia
