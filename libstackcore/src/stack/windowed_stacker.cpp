#include "stackcore/windowed_stacker.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

#include "stackcore/resample.hpp"

namespace stackcore {
namespace {

// 重みがこれ未満の画素は「実質どのAPからも支えられていない」とみなす。
// 割り算で小さな重みを分母にすると、わずかな寄与がノイズごと増幅される。
constexpr float kMinWeight = 1e-3f;

// 周期版Hann窓を50%ずつ重ねた完全な格子では、窓の重み和は1になる。
// 自動AP配置で格子が疎になった外周では1未満になるため、この値を
// 局所スタックと参照画像を混ぜるときの「完全被覆」として使う。
constexpr float kFullHannSupport = 1.0f;

}  // namespace

WindowedStacker::WindowedStacker(int width, int height, int channels, int ap_size, double scale,
                                 double pixfrac, StackMode mode, double sigma_threshold)
    : channels_(channels),
      ap_size_(ap_size),
      scale_(scale),
      pixfrac_(pixfrac),
      mode_(mode),
      sigma_threshold_(sigma_threshold) {
    if (width <= 0 || height <= 0 || channels <= 0 || ap_size < 8) {
        throw std::invalid_argument("WindowedStacker: 引数が不正です");
    }
    if (!(scale > 0.0) || scale > 8.0) {
        throw std::invalid_argument("WindowedStacker: 倍率は0より大きく8以下です");
    }
    if (!(pixfrac > 0.0) || pixfrac > 1.0) {
        throw std::invalid_argument("WindowedStacker: pixfrac は0より大きく1以下です");
    }
    if (!(sigma_threshold > 0.0)) {
        throw std::invalid_argument("WindowedStacker: σクリップ閾値は0より大きい必要があります");
    }

    drizzle_ = std::fabs(scale - 1.0) > 1e-9;
    out_width_ = static_cast<int>(std::lround(width * scale));
    out_height_ = static_cast<int>(std::lround(height * scale));
    ap_out_ = static_cast<int>(std::lround(ap_size * scale));

    // Hann窓（2次元は1次元の直積）。中心で1、縁で0。
    // 端点をちょうど0にすると、その列・行が完全に無駄になるうえ
    // 50%オーバーラップでの重み和が一様から外れる。
    // 標準的な周期版（分母を N とする）を使う。
    //
    // Drizzleでは出力グリッド上のAPサイズで作る。窓の形は座標系が変わっても
    // 「AP中心で1、縁で0」でなければならない。
    window_.resize(static_cast<std::size_t>(ap_out_) * ap_out_);
    std::vector<float> w1(static_cast<std::size_t>(ap_out_));
    for (int i = 0; i < ap_out_; ++i) {
        w1[static_cast<std::size_t>(i)] =
            static_cast<float>(0.5 * (1.0 - std::cos(2.0 * M_PI * i / ap_out_)));
    }
    for (int y = 0; y < ap_out_; ++y) {
        for (int x = 0; x < ap_out_; ++x) {
            window_[static_cast<std::size_t>(y) * ap_out_ + x] =
                w1[static_cast<std::size_t>(y)] * w1[static_cast<std::size_t>(x)];
        }
    }

    const std::size_t pixels = static_cast<std::size_t>(out_width_) * out_height_;
    sum_.assign(pixels * static_cast<std::size_t>(channels), 0.0f);
    weight_.assign(pixels, 0.0f);

}

void WindowedStacker::start_ap(Ap& ap, int center_x, int center_y) const {
    ap.cx = center_x;
    ap.cy = center_y;
    // 出力グリッド上でのAP左上。等倍なら従来どおり cx - half。
    ap.ox = static_cast<int>(std::lround((center_x - ap_size_ / 2) * scale_));
    ap.oy = static_cast<int>(std::lround((center_y - ap_size_ / 2) * scale_));
    ap.frames = 0;
    ap.weight_sum = 0.0;
    const std::size_t ap_pixels = static_cast<std::size_t>(ap_out_) * ap_out_;
    ap.accum.assign(ap_pixels * static_cast<std::size_t>(channels_), 0.0);
    if (drizzle_ || mode_ == StackMode::SigmaClip) ap.coverage.assign(ap_pixels, 0.0);
    else ap.coverage.clear();
    ap.samples.clear();
    ap.patch.clear();
    ap.frame_accum.clear();
    ap.frame_coverage.clear();
}

std::size_t WindowedStacker::ap_bytes(int frames) const {
    const std::size_t ap_pixels = static_cast<std::size_t>(ap_out_) * ap_out_;
    std::size_t bytes = ap_pixels * static_cast<std::size_t>(channels_) * sizeof(double);
    if (drizzle_ || mode_ == StackMode::SigmaClip) bytes += ap_pixels * sizeof(double);
    if (mode_ == StackMode::SigmaClip) {
        bytes += static_cast<std::size_t>(std::max(0, frames)) * channels_ * ap_pixels * sizeof(float);
    }
    bytes += static_cast<std::size_t>(ap_size_) * ap_size_ * sizeof(float);
    if (drizzle_ && mode_ == StackMode::SigmaClip) {
        bytes += ap_pixels * static_cast<std::size_t>(channels_ + 1) * sizeof(double);
    }
    return bytes;
}

void WindowedStacker::begin_ap(int center_x, int center_y) {
    if (in_ap_) throw std::logic_error("WindowedStacker: begin_ap が入れ子になっています");
    start_ap(current_, center_x, center_y);
    in_ap_ = true;
}

void WindowedStacker::add_frame(const FrameBuffer& frame, double dx, double dy, double gain,
                                double sample_weight) {
    if (!in_ap_) throw std::logic_error("WindowedStacker: begin_ap が呼ばれていません");
    accumulate(current_, frame, dx, dy, gain, sample_weight);
}

void WindowedStacker::accumulate(Ap& ap, const FrameBuffer& frame, double dx, double dy,
                                 double gain, double sample_weight) const {
    if (frame.channels() != channels_) {
        throw std::invalid_argument("WindowedStacker: チャンネル数が一致しません");
    }
    if (!(sample_weight > 0.0) || !std::isfinite(sample_weight)) {
        throw std::invalid_argument("WindowedStacker: フレーム重みは正の有限値が必要です");
    }
    if (drizzle_) {
        add_frame_drizzle(ap, frame, dx, dy, gain, sample_weight);
    } else {
        add_frame_lanczos(ap, frame, dx, dy, gain, sample_weight);
    }
    if (!drizzle_ && mode_ != StackMode::SigmaClip) ap.weight_sum += sample_weight;
    ++ap.frames;
}

void WindowedStacker::add_frame_lanczos(Ap& ap, const FrameBuffer& frame, double dx, double dy,
                                        double gain, double sample_weight) const {
    const int half = ap_size_ / 2;
    // 参照座標系でのAP左上に、そのフレームでの局所変位を足した位置を切り出す。
    const double x0 = static_cast<double>(ap.cx - half) + dx;
    const double y0 = static_cast<double>(ap.cy - half) + dy;
    const std::size_t n = static_cast<std::size_t>(ap_size_) * ap_size_;
    if (ap.patch.size() < n) ap.patch.assign(n, 0.0f);

    for (int c = 0; c < channels_; ++c) {
        resample_lanczos3(frame.plane(c), frame.width(), frame.height(), frame.stride(), x0, y0,
                          ap.patch.data(), ap_size_, ap_size_,
                          static_cast<std::size_t>(ap_size_));

        if (mode_ == StackMode::SigmaClip) {
            const std::size_t base =
                (static_cast<std::size_t>(ap.frames) * channels_ + c) * n;
            if (ap.samples.size() < base + n) ap.samples.resize(base + n);
            for (std::size_t i = 0; i < n; ++i) {
                ap.samples[base + i] = static_cast<float>(ap.patch[i] * gain);
            }
        } else {
            double* acc = ap.accum.data() + static_cast<std::size_t>(c) * n;
            for (std::size_t i = 0; i < n; ++i) {
                acc[i] += static_cast<double>(ap.patch[i]) * gain * sample_weight;
            }
        }
    }
}

void WindowedStacker::add_frame_drizzle(Ap& ap, const FrameBuffer& frame, double dx, double dy,
                                        double gain, double sample_weight) const {
    // 入力画素を「面積を持った四角」としてAP内の拡大グリッドへ落とす（仕様書 §4.9）。
    //
    // 座標の対応:
    //   フレーム座標 ix ↔ 参照座標 (ix - dx) ↔ AP内座標 (ix - dx - (cx - half))
    //   → 出力グリッドでは × scale
    const int half = ap_size_ / 2;
    const double origin_x = static_cast<double>(ap.cx - half) + dx;
    const double origin_y = static_cast<double>(ap.cy - half) + dy;
    const double half_drop = pixfrac_ * 0.5;
    const std::size_t ap_pixels = static_cast<std::size_t>(ap_out_) * ap_out_;

    if (mode_ == StackMode::SigmaClip) {
        ap.frame_accum.assign(ap_pixels * static_cast<std::size_t>(channels_), 0.0);
        ap.frame_coverage.assign(ap_pixels, 0.0);
    }

    // このAPが必要とするフレーム側の範囲。少し広めに取る。
    const int ix_begin = std::max(0, static_cast<int>(std::floor(origin_x)) - 1);
    const int ix_end = std::min(frame.width(), static_cast<int>(std::ceil(origin_x + ap_size_)) + 1);
    const int iy_begin = std::max(0, static_cast<int>(std::floor(origin_y)) - 1);
    const int iy_end =
        std::min(frame.height(), static_cast<int>(std::ceil(origin_y + ap_size_)) + 1);

    for (int iy = iy_begin; iy < iy_end; ++iy) {
        const double cy = (static_cast<double>(iy) + 0.5) - origin_y;
        const double y0 = (cy - half_drop) * scale_;
        const double y1 = (cy + half_drop) * scale_;
        int oy0 = static_cast<int>(std::floor(y0));
        int oy1 = static_cast<int>(std::ceil(y1));
        if (oy1 <= 0 || oy0 >= ap_out_) continue;
        oy0 = std::max(0, oy0);
        oy1 = std::min(ap_out_, oy1);

        for (int ix = ix_begin; ix < ix_end; ++ix) {
            const double cx = (static_cast<double>(ix) + 0.5) - origin_x;
            const double x0 = (cx - half_drop) * scale_;
            const double x1 = (cx + half_drop) * scale_;
            int ox0 = static_cast<int>(std::floor(x0));
            int ox1 = static_cast<int>(std::ceil(x1));
            if (ox1 <= 0 || ox0 >= ap_out_) continue;
            ox0 = std::max(0, ox0);
            ox1 = std::min(ap_out_, ox1);

            for (int oy = oy0; oy < oy1; ++oy) {
                const double overlap_y = std::min(y1, static_cast<double>(oy + 1)) -
                                         std::max(y0, static_cast<double>(oy));
                if (overlap_y <= 0.0) continue;
                for (int ox = ox0; ox < ox1; ++ox) {
                    const double overlap_x = std::min(x1, static_cast<double>(ox + 1)) -
                                             std::max(x0, static_cast<double>(ox));
                    if (overlap_x <= 0.0) continue;

                    const double area = overlap_x * overlap_y;
                    const std::size_t index = static_cast<std::size_t>(oy) * ap_out_ + ox;
                    if (mode_ == StackMode::SigmaClip) {
                        ap.frame_coverage[index] += area;
                    } else {
                        ap.coverage[index] += area * sample_weight;
                    }
                    for (int c = 0; c < channels_; ++c) {
                        const double contribution =
                            static_cast<double>(frame.row(c, iy)[ix]) * gain * area;
                        if (mode_ == StackMode::SigmaClip) {
                            ap.frame_accum[static_cast<std::size_t>(c) * ap_pixels + index] +=
                                contribution;
                        } else {
                            ap.accum[static_cast<std::size_t>(c) * ap_pixels + index] +=
                                contribution * sample_weight;
                        }
                    }
                }
            }
        }
    }

    if (mode_ == StackMode::SigmaClip) {
        const float missing = std::numeric_limits<float>::quiet_NaN();
        const std::size_t base = static_cast<std::size_t>(ap.frames) * channels_ * ap_pixels;
        ap.samples.resize(base + static_cast<std::size_t>(channels_) * ap_pixels, missing);
        for (std::size_t i = 0; i < ap_pixels; ++i) {
            if (ap.frame_coverage[i] <= 0.0) continue;
            for (int c = 0; c < channels_; ++c) {
                ap.samples[base + static_cast<std::size_t>(c) * ap_pixels + i] =
                    static_cast<float>(ap.frame_accum[static_cast<std::size_t>(c) * ap_pixels + i] /
                                       ap.frame_coverage[i]);
            }
        }
    }
}

void WindowedStacker::prepare_sigma_ap(Ap& ap) const {
    std::fill(ap.accum.begin(), ap.accum.end(), 0.0);
    std::fill(ap.coverage.begin(), ap.coverage.end(), 0.0);
    const std::size_t pixels = static_cast<std::size_t>(ap_out_) * ap_out_;
    const int frames_in_ap = ap.frames;
    const std::vector<float>& samples = ap.samples;
    std::vector<float> values;
    std::vector<float> deviations;
    values.reserve(static_cast<std::size_t>(frames_in_ap));
    deviations.reserve(static_cast<std::size_t>(frames_in_ap));

    for (std::size_t i = 0; i < pixels; ++i) {
        values.clear();
        for (int frame = 0; frame < frames_in_ap; ++frame) {
            const std::size_t base = static_cast<std::size_t>(frame) * channels_ * pixels;
            float luma;
            if (channels_ == 1) {
                luma = samples[base + i];
            } else {
                const float r = samples[base + i];
                const float g = samples[base + pixels + i];
                const float b = samples[base + 2 * pixels + i];
                luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            }
            if (std::isfinite(luma)) values.push_back(luma);
        }
        if (values.empty()) continue;

        std::sort(values.begin(), values.end());
        const float median = values[values.size() / 2];
        deviations.clear();
        for (float value : values) deviations.push_back(std::fabs(value - median));
        std::sort(deviations.begin(), deviations.end());
        const double robust_sigma = 1.4826 * deviations[deviations.size() / 2];
        const double threshold = std::max(1e-4, sigma_threshold_ * robust_sigma);

        for (int frame = 0; frame < frames_in_ap; ++frame) {
            const std::size_t base = static_cast<std::size_t>(frame) * channels_ * pixels;
            float luma;
            if (channels_ == 1) {
                luma = samples[base + i];
            } else {
                luma = 0.2126f * samples[base + i] +
                       0.7152f * samples[base + pixels + i] +
                       0.0722f * samples[base + 2 * pixels + i];
            }
            if (!std::isfinite(luma) || std::fabs(luma - median) > threshold) continue;
            ap.coverage[i] += 1.0;
            for (int c = 0; c < channels_; ++c) {
                ap.accum[static_cast<std::size_t>(c) * pixels + i] +=
                    samples[base + static_cast<std::size_t>(c) * pixels + i];
            }
        }
    }
}

void WindowedStacker::end_ap() {
    if (!in_ap_) throw std::logic_error("WindowedStacker: begin_ap が呼ばれていません");
    in_ap_ = false;
    commit_ap(current_);
}

void WindowedStacker::commit_ap(Ap& ap) {
    contributions_ += ap.frames;
    if (ap.frames == 0) return;

    if (mode_ == StackMode::SigmaClip) prepare_sigma_ap(ap);

    const double inv_weight = ap.weight_sum > 0.0 ? 1.0 / ap.weight_sum : 0.0;
    const std::size_t out_pixels = static_cast<std::size_t>(out_width_) * out_height_;
    const std::size_t ap_pixels = static_cast<std::size_t>(ap_out_) * ap_out_;

    // AP内の平均を取ってから、窓を掛けて全体へ足す。
    // 全体バッファへの加算はAP1枚につき1回しか起きないので、
    // ここから先が float32 でも誤差は問題にならない。
    //
    // Drizzleでは「フレーム数で割る」のではなく「被覆量で割る」。
    // 出力画素ごとに落ちてきた面積が違うため。
    for (int ay = 0; ay < ap_out_; ++ay) {
        const int oy = ap.oy + ay;
        if (oy < 0 || oy >= out_height_) continue;
        for (int ax = 0; ax < ap_out_; ++ax) {
            const int ox = ap.ox + ax;
            if (ox < 0 || ox >= out_width_) continue;

            const std::size_t ap_index = static_cast<std::size_t>(ay) * ap_out_ + ax;
            const float w = window_[ap_index];
            if (w <= 0.0f) continue;

            double norm;
            if (drizzle_ || mode_ == StackMode::SigmaClip) {
                const double coverage = ap.coverage[ap_index];
                // 一度も落ちてこなかった出力画素。窓の重みも足さない
                // （足すと分母だけ増えて暗くなる）。
                if (coverage <= 0.0) continue;
                norm = 1.0 / coverage;
            } else {
                norm = inv_weight;
            }

            const std::size_t out_index = static_cast<std::size_t>(oy) * out_width_ + ox;
            weight_[out_index] += w;

            for (int c = 0; c < channels_; ++c) {
                const double mean =
                    ap.accum[static_cast<std::size_t>(c) * ap_pixels + ap_index] * norm;
                sum_[static_cast<std::size_t>(c) * out_pixels + out_index] +=
                    static_cast<float>(mean) * w;
            }
        }
    }
    ++ap_count_;
    // 使い終わったAPのメモリを手放す（多くのAPを同時に持つときに効く）。
    std::vector<double>().swap(ap.accum);
    std::vector<double>().swap(ap.coverage);
    std::vector<float>().swap(ap.samples);
    std::vector<float>().swap(ap.patch);
    std::vector<double>().swap(ap.frame_accum);
    std::vector<double>().swap(ap.frame_coverage);
}

void WindowedStacker::finish(FrameBuffer& out, WindowedStackStats& stats,
                             const FrameBuffer* fallback) const {
    out.reset(out_width_, out_height_, channels_);

    stats = WindowedStackStats{};
    stats.ap_count = ap_count_;
    stats.contributions = contributions_;
    stats.min_weight = weight_.empty() ? 0.0 : weight_[0];
    stats.max_weight = 0.0;

    const std::size_t pixels = static_cast<std::size_t>(out_width_) * out_height_;

    // 未被覆画素を埋める画像。出力が拡大されている場合は拡大して使う。
    // ここを0にすると、グローバルスタックには写っていた淡い部分が黒く抜ける。
    FrameBuffer scaled_fallback;
    const FrameBuffer* fill = nullptr;
    if (fallback != nullptr && !fallback->empty() && fallback->channels() == channels_) {
        if (fallback->width() == out_width_ && fallback->height() == out_height_) {
            fill = fallback;
        } else {
            scaled_fallback.reset(out_width_, out_height_, channels_);
            const double sx_scale = static_cast<double>(fallback->width()) / out_width_;
            const double sy_scale = static_cast<double>(fallback->height()) / out_height_;
            for (int c = 0; c < channels_; ++c) {
                for (int y = 0; y < out_height_; ++y) {
                    const double sy = (y + 0.5) * sy_scale - 0.5;
                    float* d = scaled_fallback.row(c, y);
                    for (int x = 0; x < out_width_; ++x) {
                        const double sx = (x + 0.5) * sx_scale - 0.5;
                        float value = 0.0f;
                        resample_lanczos3(fallback->plane(c), fallback->width(),
                                          fallback->height(), fallback->stride(), sx, sy,
                                          &value, 1, 1, 1);
                        d[x] = value;
                    }
                }
            }
            scaled_fallback.invalidate_luma();
            fill = &scaled_fallback;
        }
    }

    for (int y = 0; y < out_height_; ++y) {
        for (int x = 0; x < out_width_; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * out_width_ + x;
            const float w = weight_[i];
            if (w < stats.min_weight) stats.min_weight = w;
            if (w > stats.max_weight) stats.max_weight = w;

            if (w < kMinWeight) {
                if (w <= 0.0f) {
                    ++stats.uncovered_pixels;
                } else {
                    ++stats.weak_pixels;
                }
                for (int c = 0; c < channels_; ++c) {
                    out.row(c, y)[x] = fill != nullptr ? fill->row(c, y)[x] : 0.0f;
                }
                continue;
            }

            const float inv_w = 1.0f / w;
            const float local_alpha =
                fill != nullptr ? std::clamp(w / kFullHannSupport, 0.0f, 1.0f) : 1.0f;
            if (fill != nullptr && local_alpha < 1.0f) {
                ++stats.fallback_blended_pixels;
            }
            for (int c = 0; c < channels_; ++c) {
                const double local =
                    static_cast<double>(sum_[static_cast<std::size_t>(c) * pixels + i]) * inv_w;
                double v = local;
                if (fill != nullptr && local_alpha < 1.0f) {
                    v = local * local_alpha +
                        static_cast<double>(fill->row(c, y)[x]) * (1.0f - local_alpha);
                }
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
