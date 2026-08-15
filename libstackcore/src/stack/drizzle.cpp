#include "stackcore/drizzle.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace stackcore {

Drizzle::Drizzle(int in_width, int in_height, int channels, double scale, double pixfrac)
    : in_width_(in_width),
      in_height_(in_height),
      channels_(channels),
      scale_(scale),
      pixfrac_(pixfrac) {
    if (in_width <= 0 || in_height <= 0 || channels <= 0) {
        throw std::invalid_argument("Drizzle: 寸法が不正です");
    }
    if (!(scale > 0.0) || scale > 8.0) {
        throw std::invalid_argument("Drizzle: 倍率は0より大きく8以下です");
    }
    if (!(pixfrac > 0.0) || pixfrac > 1.0) {
        throw std::invalid_argument("Drizzle: pixfrac は0より大きく1以下です");
    }

    out_width_ = static_cast<int>(std::lround(in_width * scale));
    out_height_ = static_cast<int>(std::lround(in_height * scale));

    const std::size_t pixels = static_cast<std::size_t>(out_width_) * out_height_;
    sum_.assign(pixels * static_cast<std::size_t>(channels), 0.0);
    weight_.assign(pixels, 0.0);
}

void Drizzle::add(const FrameBuffer& frame, double dx, double dy, double gain) {
    if (frame.width() != in_width_ || frame.height() != in_height_ ||
        frame.channels() != channels_) {
        throw std::invalid_argument("Drizzle: 入力フレームの寸法が一致しません");
    }

    // 入力画素 (ix, iy) は入力座標で [ix, ix+1) を占める。
    // pixfrac ぶん中心に向かって縮め、変位を足し、出力グリッドへ写す。
    const double half = pixfrac_ * 0.5;
    const std::size_t out_pixels = static_cast<std::size_t>(out_width_) * out_height_;

    for (int iy = 0; iy < in_height_; ++iy) {
        // 出力座標での、この行の落とし込み範囲。
        const double cy = static_cast<double>(iy) + 0.5 + dy;
        const double y0 = (cy - half) * scale_;
        const double y1 = (cy + half) * scale_;
        int oy0 = static_cast<int>(std::floor(y0));
        int oy1 = static_cast<int>(std::ceil(y1));
        if (oy1 <= 0 || oy0 >= out_height_) continue;
        oy0 = std::max(0, oy0);
        oy1 = std::min(out_height_, oy1);

        for (int ix = 0; ix < in_width_; ++ix) {
            const double cx = static_cast<double>(ix) + 0.5 + dx;
            const double x0 = (cx - half) * scale_;
            const double x1 = (cx + half) * scale_;
            int ox0 = static_cast<int>(std::floor(x0));
            int ox1 = static_cast<int>(std::ceil(x1));
            if (ox1 <= 0 || ox0 >= out_width_) continue;
            ox0 = std::max(0, ox0);
            ox1 = std::min(out_width_, ox1);

            for (int oy = oy0; oy < oy1; ++oy) {
                // 出力画素 oy が占める [oy, oy+1) と落とし込み範囲の重なり。
                const double overlap_y =
                    std::min(y1, static_cast<double>(oy + 1)) - std::max(y0, static_cast<double>(oy));
                if (overlap_y <= 0.0) continue;

                for (int ox = ox0; ox < ox1; ++ox) {
                    const double overlap_x =
                        std::min(x1, static_cast<double>(ox + 1)) -
                        std::max(x0, static_cast<double>(ox));
                    if (overlap_x <= 0.0) continue;

                    // 重み＝重なりの面積。これがそのまま「その出力画素が
                    // この入力画素からどれだけ情報をもらったか」になる。
                    const double area = overlap_x * overlap_y;
                    const std::size_t index = static_cast<std::size_t>(oy) * out_width_ + ox;
                    weight_[index] += area;
                    for (int c = 0; c < channels_; ++c) {
                        sum_[static_cast<std::size_t>(c) * out_pixels + index] +=
                            static_cast<double>(frame.row(c, iy)[ix]) * gain * area;
                    }
                }
            }
        }
    }
    ++frames_;
}

void Drizzle::finish(FrameBuffer& out, DrizzleStats& stats) const {
    out.reset(out_width_, out_height_, channels_);

    stats = DrizzleStats{};
    stats.frames = frames_;
    stats.min_weight = weight_.empty() ? 0.0 : weight_[0];

    const std::size_t out_pixels = static_cast<std::size_t>(out_width_) * out_height_;
    for (int y = 0; y < out_height_; ++y) {
        for (int x = 0; x < out_width_; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * out_width_ + x;
            const double w = weight_[index];
            if (w < stats.min_weight) stats.min_weight = w;
            if (w > stats.max_weight) stats.max_weight = w;

            if (w <= 0.0) {
                // どのフレームからも情報が落ちてこなかった出力画素。
                // pixfrac を小さくしすぎたか、変位の分布が偏っている。
                ++stats.uncovered_pixels;
                for (int c = 0; c < channels_; ++c) out.row(c, y)[x] = 0.0f;
                continue;
            }

            const double inv = 1.0 / w;
            for (int c = 0; c < channels_; ++c) {
                double v = sum_[static_cast<std::size_t>(c) * out_pixels + index] * inv;
                if (v > stats.max_value) stats.max_value = v;
                if (v > 1.0) {
                    ++stats.clipped;
                    v = 1.0;
                } else if (v < 0.0) {
                    v = 0.0;
                }
                out.row(c, y)[x] = static_cast<float>(v);
            }
        }
    }
    out.invalidate_luma();
}

}  // namespace stackcore
