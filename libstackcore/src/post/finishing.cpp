#include "stackcore/finishing.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "stackcore/resample.hpp"

#include "../common/parallel_rows.hpp"

namespace stackcore {
namespace {

void copy_frame(const FrameBuffer& src, FrameBuffer& out) {
    if (&src == &out) return;
    if (out.width() != src.width() || out.height() != src.height() ||
        out.channels() != src.channels()) {
        out.reset(src.width(), src.height(), src.channels());
    }
    out.set_source_bit_depth(src.source_bit_depth());
    for (int c = 0; c < src.channels(); ++c) {
        detail::parallel_rows(src.height(), [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                std::copy(src.row(c, y), src.row(c, y) + src.width(), out.row(c, y));
            }
        }, 32);
    }
    out.invalidate_luma();
}

// 画素値の分位点（0..1）。決定論的に nth_element で求める。
double percentile(std::vector<float> values, double p) {
    if (values.empty()) return 0.0;
    const std::size_t k = std::min(values.size() - 1,
                                   static_cast<std::size_t>(p * (values.size() - 1) + 0.5));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
    return values[k];
}

// 輝度（Rec.709）。1chならそのまま。
std::vector<float> luma_plane(const FrameBuffer& image) {
    const int w = image.width(), h = image.height();
    std::vector<float> out(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float v;
            if (image.channels() >= 3) {
                v = 0.2126f * image.row(0, y)[x] + 0.7152f * image.row(1, y)[x] +
                    0.0722f * image.row(2, y)[x];
            } else {
                v = image.row(0, y)[x];
            }
            out[static_cast<std::size_t>(y) * w + x] = v;
        }
    }
    return out;
}

// 背景と対象を分けるしきい値。背景の中央値と明部の分位点の間に置く。
double object_threshold(const std::vector<float>& luma) {
    const double background = percentile(luma, 0.10);
    const double bright = percentile(luma, 0.995);
    return background + 0.2 * (bright - background);
}

// 半径 r の最小・最大フィルタ（分離可能。端は範囲を縮める）。行ごとに並列に回す。
void min_max_filter(const float* src, int w, int h, int r, std::vector<float>& lo,
                    std::vector<float>& hi) {
    const std::size_t n = static_cast<std::size_t>(w) * h;
    std::vector<float> tlo(n), thi(n);
    detail::parallel_rows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* s = src + static_cast<std::size_t>(y) * w;
            for (int x = 0; x < w; ++x) {
                float a = s[x], b = s[x];
                for (int k = std::max(0, x - r); k <= std::min(w - 1, x + r); ++k) {
                    a = std::min(a, s[k]);
                    b = std::max(b, s[k]);
                }
                tlo[static_cast<std::size_t>(y) * w + x] = a;
                thi[static_cast<std::size_t>(y) * w + x] = b;
            }
        }
    });
    lo.assign(n, 0.0f);
    hi.assign(n, 0.0f);
    detail::parallel_rows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const int k0 = std::max(0, y - r), k1 = std::min(h - 1, y + r);
            float* dlo = lo.data() + static_cast<std::size_t>(y) * w;
            float* dhi = hi.data() + static_cast<std::size_t>(y) * w;
            const float* slo = tlo.data() + static_cast<std::size_t>(y) * w;
            const float* shi = thi.data() + static_cast<std::size_t>(y) * w;
            for (int x = 0; x < w; ++x) {
                dlo[x] = slo[x];
                dhi[x] = shi[x];
            }
            for (int k = k0; k <= k1; ++k) {
                const float* rlo = tlo.data() + static_cast<std::size_t>(k) * w;
                const float* rhi = thi.data() + static_cast<std::size_t>(k) * w;
                for (int x = 0; x < w; ++x) {
                    dlo[x] = std::min(dlo[x], rlo[x]);
                    dhi[x] = std::max(dhi[x], rhi[x]);
                }
            }
        }
    });
}

}  // namespace

// ---- チャンネル合わせ --------------------------------------------------------

ChannelOffsets estimate_channel_offsets(const FrameBuffer& rgb, int max_shift) {
    ChannelOffsets result;
    if (rgb.channels() != 3 || rgb.empty()) return result;
    const int w = rgb.width(), h = rgb.height();
    max_shift = std::max(1, std::min(max_shift, std::min(w, h) / 4));

    // 対象の明るい領域の重心を中心に、最大512×512の窓で比べる。
    // 背景だけの広い範囲を入れると、ノイズ同士の相関で推定がぶれる。
    const std::vector<float> luma = luma_plane(rgb);
    const double threshold = object_threshold(luma);
    double sx = 0.0, sy = 0.0, sw = 0.0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double v = luma[static_cast<std::size_t>(y) * w + x] - threshold;
            if (v > 0.0) {
                sx += v * x;
                sy += v * y;
                sw += v;
            }
        }
    }
    const int cx = sw > 0.0 ? static_cast<int>(sx / sw) : w / 2;
    const int cy = sw > 0.0 ? static_cast<int>(sy / sw) : h / 2;
    const int half = std::min(256, std::min(w, h) / 2 - max_shift - 1);
    if (half < 8) return result;
    const int x0 = std::max(max_shift, std::min(cx - half, w - max_shift - 2 * half));
    const int y0 = std::max(max_shift, std::min(cy - half, h - max_shift - 2 * half));
    const int size = 2 * half;

    const auto estimate = [&](int channel, double& out_dx, double& out_dy) {
        // 基準（G）の窓の平均と分散。
        double gm = 0.0;
        for (int y = 0; y < size; ++y) {
            const float* g = rgb.row(1, y0 + y) + x0;
            for (int x = 0; x < size; ++x) gm += g[x];
        }
        gm /= static_cast<double>(size) * size;
        double gv = 0.0;
        for (int y = 0; y < size; ++y) {
            const float* g = rgb.row(1, y0 + y) + x0;
            for (int x = 0; x < size; ++x) gv += (g[x] - gm) * (g[x] - gm);
        }
        const int span = 2 * max_shift + 1;
        std::vector<double> score(static_cast<std::size_t>(span) * span, -2.0);
        int best_x = 0, best_y = 0;
        double best = -2.0;
        for (int dy = -max_shift; dy <= max_shift; ++dy) {
            for (int dx = -max_shift; dx <= max_shift; ++dx) {
                double m = 0.0;
                for (int y = 0; y < size; ++y) {
                    const float* c = rgb.row(channel, y0 + y + dy) + x0 + dx;
                    for (int x = 0; x < size; ++x) m += c[x];
                }
                m /= static_cast<double>(size) * size;
                double cov = 0.0, cv = 0.0;
                for (int y = 0; y < size; ++y) {
                    const float* g = rgb.row(1, y0 + y) + x0;
                    const float* c = rgb.row(channel, y0 + y + dy) + x0 + dx;
                    for (int x = 0; x < size; ++x) {
                        const double a = g[x] - gm, b = c[x] - m;
                        cov += a * b;
                        cv += b * b;
                    }
                }
                const double z = (gv > 0.0 && cv > 0.0) ? cov / std::sqrt(gv * cv) : -1.0;
                score[static_cast<std::size_t>(dy + max_shift) * span + (dx + max_shift)] = z;
                if (z > best) {
                    best = z;
                    best_x = dx;
                    best_y = dy;
                }
            }
        }
        // 放物線で頂点を求める（端に張り付いたら整数のまま）。
        const auto at = [&](int dx, int dy) {
            return score[static_cast<std::size_t>(dy + max_shift) * span + (dx + max_shift)];
        };
        double fx = 0.0, fy = 0.0;
        if (best_x > -max_shift && best_x < max_shift) {
            const double l = at(best_x - 1, best_y), c = at(best_x, best_y), r = at(best_x + 1, best_y);
            const double den = l - 2.0 * c + r;
            if (den < 0.0) fx = std::max(-0.5, std::min(0.5, 0.5 * (l - r) / den));
        }
        if (best_y > -max_shift && best_y < max_shift) {
            const double u = at(best_x, best_y - 1), c = at(best_x, best_y), d = at(best_x, best_y + 1);
            const double den = u - 2.0 * c + d;
            if (den < 0.0) fy = std::max(-0.5, std::min(0.5, 0.5 * (u - d) / den));
        }
        // 0.01画素に丸める。GUIの数値表示と設定の保存で値が揺れないようにする。
        out_dx = std::round((best_x + fx) * 100.0) / 100.0;
        out_dy = std::round((best_y + fy) * 100.0) / 100.0;
    };
    estimate(0, result.red_dx, result.red_dy);
    estimate(2, result.blue_dx, result.blue_dy);
    return result;
}

void shift_channels(const FrameBuffer& src, const ChannelOffsets& offsets, FrameBuffer& out) {
    if (src.channels() != 3 || !offsets.any()) {
        copy_frame(src, out);
        return;
    }
    FrameBuffer result(src.width(), src.height(), 3);
    result.set_source_bit_depth(src.source_bit_depth());
    const double dxs[3] = {offsets.red_dx, 0.0, offsets.blue_dx};
    const double dys[3] = {offsets.red_dy, 0.0, offsets.blue_dy};
    for (int c = 0; c < 3; ++c) {
        if (dxs[c] == 0.0 && dys[c] == 0.0) {
            for (int y = 0; y < src.height(); ++y) {
                std::copy(src.row(c, y), src.row(c, y) + src.width(), result.row(c, y));
            }
        } else {
            resample_lanczos3(src.plane(c), src.width(), src.height(), src.stride(), dxs[c],
                              dys[c], result.plane(c), src.width(), src.height(),
                              result.stride());
        }
    }
    result.invalidate_luma();
    out = std::move(result);
}

// ---- 色 --------------------------------------------------------------------

void estimate_white_balance(const FrameBuffer& rgb, double gains[3]) {
    gains[0] = gains[1] = gains[2] = 1.0;
    if (rgb.channels() != 3 || rgb.empty()) return;
    const std::vector<float> luma = luma_plane(rgb);
    const double threshold = object_threshold(luma);
    const double saturated = 0.98;  // 飽和した画素は色の情報を持たない
    double sum[3] = {0.0, 0.0, 0.0};
    const int w = rgb.width();
    for (int y = 0; y < rgb.height(); ++y) {
        for (int x = 0; x < w; ++x) {
            if (luma[static_cast<std::size_t>(y) * w + x] <= threshold) continue;
            const float r = rgb.row(0, y)[x], g = rgb.row(1, y)[x], b = rgb.row(2, y)[x];
            if (r >= saturated || g >= saturated || b >= saturated) continue;
            sum[0] += r;
            sum[1] += g;
            sum[2] += b;
        }
    }
    if (sum[0] <= 0.0 || sum[1] <= 0.0 || sum[2] <= 0.0) return;
    // 0.001刻みに丸める（保存・表示で値が揺れないように）。
    gains[0] = std::round(sum[1] / sum[0] * 1000.0) / 1000.0;
    gains[2] = std::round(sum[1] / sum[2] * 1000.0) / 1000.0;
}

void apply_color(const FrameBuffer& src, const ColorAdjust& color, FrameBuffer& out) {
    if (src.channels() != 3 || color.identity()) {
        copy_frame(src, out);
        return;
    }
    // 画素ごとに独立なので、src と out が同じ（その場で書き換える）でもよい。
    if (&src != &out &&
        (out.width() != src.width() || out.height() != src.height() || out.channels() != 3)) {
        out.reset(src.width(), src.height(), 3);
    }
    const double s = color.saturation;
    const int width = src.width();
    detail::parallel_rows(src.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* in[3] = {src.row(0, y), src.row(1, y), src.row(2, y)};
            float* o[3] = {out.row(0, y), out.row(1, y), out.row(2, y)};
            for (int x = 0; x < width; ++x) {
                double v[3];
                for (int c = 0; c < 3; ++c) v[c] = in[c][x] * color.gain[c];
                // 彩度は輝度を保ったまま色差を伸縮する。
                const double l = 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
                for (int c = 0; c < 3; ++c) {
                    const double t = l + s * (v[c] - l);
                    o[c][x] = static_cast<float>(t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t));
                }
            }
        }
    });
    out.set_source_bit_depth(src.source_bit_depth());
    out.invalidate_luma();
}

// ---- 形 --------------------------------------------------------------------

void detect_object_bounds(const FrameBuffer& image, int margin, int& x, int& y, int& width,
                          int& height) {
    const int w = image.width(), h = image.height();
    x = 0;
    y = 0;
    width = w;
    height = h;
    if (image.empty()) return;
    const std::vector<float> luma = luma_plane(image);
    const double threshold = object_threshold(luma);
    // 1画素だけの輝点（ホットピクセル）で矩形が広がらないよう、
    // 行・列ごとに「しきい値を超えた画素が3つ以上あるか」で判定する。
    std::vector<int> rows(static_cast<std::size_t>(h), 0), cols(static_cast<std::size_t>(w), 0);
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            if (luma[static_cast<std::size_t>(yy) * w + xx] > threshold) {
                ++rows[static_cast<std::size_t>(yy)];
                ++cols[static_cast<std::size_t>(xx)];
            }
        }
    }
    int top = -1, bottom = -1, left = -1, right = -1;
    for (int yy = 0; yy < h; ++yy) if (rows[static_cast<std::size_t>(yy)] >= 3) { if (top < 0) top = yy; bottom = yy; }
    for (int xx = 0; xx < w; ++xx) if (cols[static_cast<std::size_t>(xx)] >= 3) { if (left < 0) left = xx; right = xx; }
    if (top < 0 || left < 0) return;
    left = std::max(0, left - margin);
    top = std::max(0, top - margin);
    right = std::min(w - 1, right + margin);
    bottom = std::min(h - 1, bottom + margin);
    x = left;
    y = top;
    width = right - left + 1;
    height = bottom - top + 1;
}

void apply_geometry(const FrameBuffer& src, const Geometry& g, FrameBuffer& out) {
    if (g.identity()) {
        copy_frame(src, out);
        return;
    }
    int cx = 0, cy = 0, cw = src.width(), ch = src.height();
    if (g.crop) {
        cx = std::max(0, std::min(g.crop_x, src.width() - 1));
        cy = std::max(0, std::min(g.crop_y, src.height() - 1));
        cw = std::max(1, std::min(g.crop_width, src.width() - cx));
        ch = std::max(1, std::min(g.crop_height, src.height() - cy));
    }
    const int turns = ((g.rotate_quarter_turns % 4) + 4) % 4;
    const int ow = (turns % 2 == 0) ? cw : ch;
    const int oh = (turns % 2 == 0) ? ch : cw;
    FrameBuffer result(ow, oh, src.channels());
    result.set_source_bit_depth(src.source_bit_depth());
    for (int c = 0; c < src.channels(); ++c) {
        detail::parallel_rows(oh, [&](int y0, int y1) {
            for (int oy = y0; oy < y1; ++oy) {
                float* dst = result.row(c, oy);
                for (int ox = 0; ox < ow; ++ox) {
                    // 反転は回転後の座標で掛けるので、先に反転を戻す。
                    const int rx = g.flip_horizontal ? ow - 1 - ox : ox;
                    const int ry = g.flip_vertical ? oh - 1 - oy : oy;
                    // 時計回りの回転を戻して、クロップ後の座標 (sx, sy) を求める。
                    int sx = rx, sy = ry;
                    if (turns == 1) { sx = ry; sy = ch - 1 - rx; }
                    else if (turns == 2) { sx = cw - 1 - rx; sy = ch - 1 - ry; }
                    else if (turns == 3) { sx = cw - 1 - ry; sy = rx; }
                    dst[ox] = src.row(c, cy + sy)[cx + sx];
                }
            }
        });
    }
    result.invalidate_luma();
    out = std::move(result);
}

// ---- デリンギング ------------------------------------------------------------

namespace {

// デリンギングの許容範囲（元画像の近傍の最小・最大）。チャンネルごとに w×h の連続配列。
void dering_bounds(const FrameBuffer& original, int radius, std::vector<float>& lo,
                   std::vector<float>& hi) {
    const int w = original.width(), h = original.height();
    const std::size_t plane_size = static_cast<std::size_t>(w) * h;
    lo.assign(plane_size * original.channels(), 0.0f);
    hi.assign(plane_size * original.channels(), 0.0f);
    std::vector<float> plane(plane_size), plo, phi;
    for (int c = 0; c < original.channels(); ++c) {
        for (int y = 0; y < h; ++y) {
            std::copy(original.row(c, y), original.row(c, y) + w,
                      plane.begin() + static_cast<std::ptrdiff_t>(y) * w);
        }
        min_max_filter(plane.data(), w, h, radius, plo, phi);
        std::copy(plo.begin(), plo.end(), lo.begin() + static_cast<std::ptrdiff_t>(c * plane_size));
        std::copy(phi.begin(), phi.end(), hi.begin() + static_cast<std::ptrdiff_t>(c * plane_size));
    }
}

void apply_dering_bounds(const std::vector<float>& lo, const std::vector<float>& hi,
                         double strength, FrameBuffer& sharpened) {
    const int w = sharpened.width(), h = sharpened.height();
    const std::size_t plane_size = static_cast<std::size_t>(w) * h;
    const float keep = static_cast<float>(1.0 - strength);
    for (int c = 0; c < sharpened.channels(); ++c) {
        const float* clo = lo.data() + c * plane_size;
        const float* chi = hi.data() + c * plane_size;
        detail::parallel_rows(h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                float* d = sharpened.row(c, y);
                for (int x = 0; x < w; ++x) {
                    const float a = clo[static_cast<std::size_t>(y) * w + x];
                    const float b = chi[static_cast<std::size_t>(y) * w + x];
                    if (d[x] > b) d[x] = b + (d[x] - b) * keep;
                    else if (d[x] < a) d[x] = a - (a - d[x]) * keep;
                }
            }
        });
    }
    sharpened.invalidate_luma();
}

}  // namespace

void dering(const FrameBuffer& original, double strength, int radius, FrameBuffer& sharpened) {
    if (strength <= 0.0) return;
    if (original.width() != sharpened.width() || original.height() != sharpened.height() ||
        original.channels() != sharpened.channels()) {
        throw std::invalid_argument("デリンギング: 画像の寸法が一致しません");
    }
    std::vector<float> lo, hi;
    dering_bounds(original, std::max(1, radius), lo, hi);
    apply_dering_bounds(lo, hi, std::min(1.0, strength), sharpened);
}

// ---- 全体 ------------------------------------------------------------------

bool FinishingSettings::identity() const {
    if (channels.any() || dering > 0.0 || !color.identity() || !geometry.identity()) return false;
    if (stretch && (black != 0.0 || white != 1.0 || gamma != 1.0)) return false;
    for (const WaveletLayerParams& p : wavelet) {
        if (p.sharpen != 1.0 || p.denoise != 0.0) return false;
    }
    return true;
}

void FinishingPipeline::set_input(std::shared_ptr<const FrameBuffer> stacked, int wavelet_layers) {
    input_ = std::move(stacked);
    layers_ = wavelet_layers;
    aligned_valid_ = false;
    wavelet_valid_ = false;
    dering_radius_ = 0;
    aligned_.clear();
}

void FinishingPipeline::ensure_aligned(const ChannelOffsets& offsets) {
    if (!input_) throw std::logic_error("仕上げ: 入力がありません");
    if (aligned_valid_ && aligned_offsets_ == offsets) return;
    shift_channels(*input_, offsets, aligned_);
    aligned_offsets_ = offsets;
    aligned_valid_ = true;
    wavelet_valid_ = false;
    dering_radius_ = 0;
}

const FrameBuffer& FinishingPipeline::aligned() {
    ensure_aligned(aligned_valid_ ? aligned_offsets_ : ChannelOffsets());
    return aligned_;
}

void FinishingPipeline::render(const FinishingSettings& s, FrameBuffer& out) {
    ensure_aligned(s.channels);

    // 途中の画像は使い回しの作業領域に置く（大きな画像で毎回確保すると遅い）。
    // 形を変えないときは out に直接書いて、最後の写しを省く。
    const bool reshape = !s.geometry.identity();
    FrameBuffer& work = reshape ? work_ : out;

    bool wavelet_changes = false;
    for (const WaveletLayerParams& p : s.wavelet) {
        if (p.sharpen != 1.0 || p.denoise != 0.0) wavelet_changes = true;
    }
    if (!s.wavelet.empty() && wavelet_changes) {
        if (static_cast<int>(s.wavelet.size()) != layers_) {
            throw std::invalid_argument("仕上げ: ウェーブレットのレイヤー数が一致しません");
        }
        if (!wavelet_valid_) {
            wavelet_.analyze(aligned_, layers_);
            wavelet_valid_ = true;
        }
        wavelet_.synthesize(s.wavelet, work);
        work.set_source_bit_depth(aligned_.source_bit_depth());
        if (s.dering > 0.0) {
            // 強調している最も粗いレイヤーの広がりを半径にする（最大8画素）。
            int radius = 1;
            for (std::size_t j = 0; j < s.wavelet.size(); ++j) {
                if (s.wavelet[j].sharpen > 1.0) radius = std::min(8, 1 << j);
            }
            // 許容範囲は元画像と半径だけで決まるので、つまみを動かすあいだは使い回す。
            if (dering_radius_ != radius) {
                dering_bounds(aligned_, radius, dering_lo_, dering_hi_);
                dering_radius_ = radius;
            }
            apply_dering_bounds(dering_lo_, dering_hi_, std::min(1.0, s.dering), work);
        }
    } else {
        copy_frame(aligned_, work);
    }

    // 色と階調は画素ごとの処理なので、その場で書き換える。
    if (!s.color.identity() && work.channels() == 3) apply_color(work, s.color, work);
    if (s.stretch) stretch_histogram(work, s.black, s.white, s.gamma, work);
    if (reshape) apply_geometry(work, s.geometry, out);
    out.set_source_bit_depth(input_->source_bit_depth());
    out.invalidate_luma();
}

}  // namespace stackcore
