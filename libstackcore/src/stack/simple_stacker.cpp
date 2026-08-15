#include "stackcore/simple_stacker.hpp"

#include <stdexcept>

namespace stackcore {

SimpleStacker::SimpleStacker(int width, int height, int channels)
    : width_(width), height_(height), channels_(channels) {
    if (width <= 0 || height <= 0 || channels <= 0) {
        throw std::invalid_argument("SimpleStacker: 幅・高さ・チャンネル数は正の値である必要があります");
    }
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    accum_.assign(pixels * static_cast<std::size_t>(channels), 0.0);
    coverage_.assign(pixels, 0u);
}

void SimpleStacker::add(const FrameBuffer& frame, int dx, int dy, double gain) {
    if (frame.width() != width_ || frame.height() != height_ ||
        frame.channels() != channels_) {
        throw std::invalid_argument("SimpleStacker: 寸法の異なるフレームです");
    }

    const std::size_t pixels = static_cast<std::size_t>(width_) * height_;

    // 出力座標 (x, y) に対応する入力座標は (x - dx, y - dy)。
    // 入力側が範囲外になる画素は寄与しない（coverage を増やさない）。
    for (int y = 0; y < height_; ++y) {
        const int sy = y - dy;
        if (sy < 0 || sy >= height_) continue;
        const int x_begin = dx > 0 ? dx : 0;
        const int x_end = dx < 0 ? width_ + dx : width_;

        std::uint32_t* cov = coverage_.data() + static_cast<std::size_t>(y) * width_;
        for (int x = x_begin; x < x_end; ++x) ++cov[x];

        for (int c = 0; c < channels_; ++c) {
            const float* src = frame.row(c, sy);
            double* dst = accum_.data() + static_cast<std::size_t>(c) * pixels +
                          static_cast<std::size_t>(y) * width_;
            for (int x = x_begin; x < x_end; ++x) {
                dst[x] += static_cast<double>(src[x - dx]) * gain;
            }
        }
    }
    ++frames_;
}

void SimpleStacker::finish(FrameBuffer& out, StackStats& stats) const {
    out.reset(width_, height_, channels_);

    const std::size_t pixels = static_cast<std::size_t>(width_) * height_;
    stats = StackStats{};
    stats.frames = frames_;
    stats.min_coverage = coverage_.empty() ? 0u : coverage_[0];
    stats.max_coverage = 0u;

    for (std::size_t i = 0; i < pixels; ++i) {
        if (coverage_[i] < stats.min_coverage) stats.min_coverage = coverage_[i];
        if (coverage_[i] > stats.max_coverage) stats.max_coverage = coverage_[i];
    }

    for (int c = 0; c < channels_; ++c) {
        const double* acc = accum_.data() + static_cast<std::size_t>(c) * pixels;
        for (int y = 0; y < height_; ++y) {
            float* dst = out.row(c, y);
            const double* a = acc + static_cast<std::size_t>(y) * width_;
            const std::uint32_t* cov = coverage_.data() + static_cast<std::size_t>(y) * width_;
            for (int x = 0; x < width_; ++x) {
                if (cov[x] == 0u) {
                    dst[x] = 0.0f;
                    continue;
                }
                const double v = a[x] / static_cast<double>(cov[x]);
                if (v > stats.max_value) stats.max_value = v;
                if (v > 1.0) {
                    ++stats.clipped;
                    dst[x] = 1.0f;
                } else if (v < 0.0) {
                    dst[x] = 0.0f;
                } else {
                    dst[x] = static_cast<float>(v);
                }
            }
        }
    }
    out.invalidate_luma();
}

}  // namespace stackcore
