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
    av_opt_set_chlayout(swr_, "in_chlayout", &in_layout, 0);
    av_opt_set_int(swr_, "in_sample_rate", in_rate, 0);
    av_opt_set_sample_fmt(swr_, "in_sample_fmt", in_fmt, 0);

    av_opt_set_chlayout(swr_, "out_chlayout", &out_layout, 0);
    av_opt_set_int(swr_, "out_sample_rate", out_rate, 0);
    av_opt_set_sample_fmt(swr_, "out_sample_fmt", out_fmt, 0);

    if (out_layout.nb_channels == 1 && in_layout.nb_channels > 1) {
        std::vector<double> matrix(static_cast<size_t>(in_layout.nb_channels), 1.0 / in_layout.nb_channels);
        swr_set_matrix(swr_, matrix.data(), in_layout.nb_channels);
    }

    return swr_init(swr_) >= 0;
}

int resampler::convert(const uint8_t **in_data, int in_samples, uint8_t **out_data, int max_out_samples) {
    if (swr_ == nullptr) return -1;
    return swr_convert(swr_, out_data, max_out_samples, in_data, in_samples);
}

int resampler::get_delay(int in_rate) {
    if (swr_ == nullptr) return 0;
    return static_cast<int>(swr_get_delay(swr_, in_rate));
}

} // namespace Leticia
