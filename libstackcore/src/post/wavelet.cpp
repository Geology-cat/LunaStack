#include "stackcore/wavelet.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "../common/parallel_rows.hpp"

namespace stackcore {
namespace {

// B3スプライン 5タップ [1,4,6,4,1]/16。
const double kB3[5] = {1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0};

int clamp_index(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// レイヤー j の穴あき畳み込み。タップ間隔は 2^j。
// 分離可能なので横→縦の2パスで行う。
void atrous_convolve(const float* src, float* dst, float* scratch, int w, int h, int step) {
    // 行ごとに独立なので並列に回す（各画素の足し込み順は変えない）。
    detail::parallel_rows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* s = src + static_cast<std::size_t>(y) * w;
            float* t = scratch + static_cast<std::size_t>(y) * w;
            for (int x = 0; x < w; ++x) {
                double acc = 0.0;
                for (int k = 0; k < 5; ++k) {
                    const int sx = clamp_index(x + (k - 2) * step, 0, w - 1);
                    acc += kB3[k] * s[sx];
                }
                t[x] = static_cast<float>(acc);
            }
        }
    });
    detail::parallel_rows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* d = dst + static_cast<std::size_t>(y) * w;
            const float* rows[5];
            for (int k = 0; k < 5; ++k) {
                const int sy = clamp_index(y + (k - 2) * step, 0, h - 1);
                rows[k] = scratch + static_cast<std::size_t>(sy) * w;
            }
            for (int x = 0; x < w; ++x) {
                double acc = 0.0;
                for (int k = 0; k < 5; ++k) acc += kB3[k] * rows[k][x];
                d[x] = static_cast<float>(acc);
            }
        }
    });
}

// B3スプラインà trousで白色ノイズが各レイヤーに残る割合（Starck & Murtagh）。
//
// **レイヤーごとにMADを取って、それをそのままノイズ推定にしてはいけない。**
// 粗いレイヤーのMADは、ノイズではなく実際の構造の大きさを測ってしまう。
// それをしきい値にすると構造を削る。実測では denoise=0.8 で
// 元画像との誤差が 0.017 → 0.126 と7倍に悪化した。
//
// 正しくは、ノイズが支配的な最細レイヤーから σ を1つ推定し、
// この係数で各レイヤーへ配分する。
const double kNoisePropagation[] = {0.889, 0.200, 0.086, 0.041, 0.020,
                                    0.010, 0.005, 0.002, 0.001, 0.0005,
                                    0.00025, 0.000125};

// 中央絶対偏差から求めたσ。外れ値（＝実際の構造）に引きずられない散らばりの尺度。
double robust_sigma(const float* v, std::size_t n) {
    if (n == 0) return 0.0;
    std::vector<float> tmp(v, v + n);
    std::nth_element(tmp.begin(), tmp.begin() + n / 2, tmp.end());
    const float median = tmp[n / 2];
    for (std::size_t i = 0; i < n; ++i) tmp[i] = std::fabs(tmp[i] - median);
    std::nth_element(tmp.begin(), tmp.begin() + n / 2, tmp.end());
    return static_cast<double>(tmp[n / 2]) / 0.6745;
}

}  // namespace

WaveletSharpener::WaveletSharpener() = default;

void WaveletSharpener::analyze(const FrameBuffer& src, int layers) {
    if (src.empty()) throw std::invalid_argument("ウェーブレット: 空のフレームです");
    if (layers < 1 || layers > 12) {
        throw std::invalid_argument("ウェーブレット: レイヤー数は1..12です");
    }

    width_ = src.width();
    height_ = src.height();
    channels_ = src.channels();
    layers_ = layers;
    plane_ = static_cast<std::size_t>(width_) * height_;

    const std::size_t total = plane_ * static_cast<std::size_t>(channels_);
    detail_.assign(static_cast<std::size_t>(layers), std::vector<float>(total, 0.0f));
    noise_.assign(static_cast<std::size_t>(layers), 0.0);

    std::vector<float> current(total), next(total), scratch(plane_);

    // stride を畳んだ連続配列に写す（FrameBuffer は行が32バイト境界に揃っている）。
    for (int c = 0; c < channels_; ++c) {
        for (int y = 0; y < height_; ++y) {
            const float* s = src.row(c, y);
            float* d = current.data() + static_cast<std::size_t>(c) * plane_ +
                       static_cast<std::size_t>(y) * width_;
            for (int x = 0; x < width_; ++x) d[x] = s[x];
        }
    }

    // 再構成は「元画像 + 変えたレイヤーの差分」で行うので、元画像を持っておく
    // （最も粗い残差 c_J の代わり。I = c_J + Σ w_j なので情報は同じ）。
    base_ = current;

    for (int j = 0; j < layers; ++j) {
        const int step = 1 << j;
        for (int c = 0; c < channels_; ++c) {
            const float* cur = current.data() + static_cast<std::size_t>(c) * plane_;
            float* nx = next.data() + static_cast<std::size_t>(c) * plane_;
            atrous_convolve(cur, nx, scratch.data(), width_, height_, step);

            // w_j = c_{j-1} - c_j
            float* w = detail_[static_cast<std::size_t>(j)].data() +
                       static_cast<std::size_t>(c) * plane_;
            const int width = width_;
            detail::parallel_rows(height_, [&](int y0, int y1) {
                const std::size_t i0 = static_cast<std::size_t>(y0) * width;
                const std::size_t i1 = static_cast<std::size_t>(y1) * width;
                for (std::size_t i = i0; i < i1; ++i) w[i] = cur[i] - nx[i];
            });
        }
        current.swap(next);
    }

    // ノイズは最細レイヤーから1つだけ推定する。
    // 先頭チャンネルで代表させるのは、チャンネルごとに変えると
    // 色によってDenoiseの効き方が変わってしまうため。
    const double sigma_finest = robust_sigma(detail_[0].data(), plane_);
    const double sigma_noise = sigma_finest / kNoisePropagation[0];
    for (int j = 0; j < layers; ++j) {
        noise_[static_cast<std::size_t>(j)] =
            sigma_noise * kNoisePropagation[static_cast<std::size_t>(j)];
    }
}

double WaveletSharpener::layer_noise(int layer) const {
    if (layer < 0 || layer >= layers_) return 0.0;
    return noise_[static_cast<std::size_t>(layer)];
}

void WaveletSharpener::synthesize(const std::vector<WaveletLayerParams>& params,
                                  FrameBuffer& out) const {
    if (layers_ == 0) throw std::logic_error("ウェーブレット: 先に analyze を呼んでください");
    if (static_cast<int>(params.size()) != layers_) {
        throw std::invalid_argument("ウェーブレット: パラメータ数がレイヤー数と一致しません");
    }
    for (const WaveletLayerParams& p : params) {
        if (!std::isfinite(p.sharpen) || p.sharpen < 0.0 ||
            p.sharpen > kWaveletSharpenMaximum) {
            throw std::invalid_argument("ウェーブレット: Sharpen係数は0〜99の有限値です");
        }
        if (!std::isfinite(p.denoise) || p.denoise < 0.0 || p.denoise > 1.0) {
            throw std::invalid_argument("ウェーブレット: Denoiseは0〜1の有限値です");
        }
    }

    if (out.width() != width_ || out.height() != height_ || out.channels() != channels_) {
        out.reset(width_, height_, channels_);
    }

    // I' = c_J + Σ g_j·w'_j を、I = c_J + Σ w_j を使って
    //   I' = I + Σ (g_j·w'_j − w_j)
    // と書き直して計算する（w'_j はしきい値を掛けたレイヤー）。変えていないレイヤー
    // （強調1.0・ノイズ0）は差分が0なので読まなくてよい。つまみを動かすときに読むのは
    // 元画像と変えたレイヤーだけになり、大きな画像でも速く描ける。すべて初期値なら
    // 出力は入力と一致する。差分の和は倍精度で、レイヤーの順に足す（結果は一意）。
    //
    // しきい値はレイヤーのノイズ推定に対する比。
    // 絶対値で持つと、画像の明るさやビット深度で意味が変わる。
    //
    // 上限係数は実測で決めた。最初 3.0（＝3σまで削る）にしたところ、
    // 誤差が最小になるのがスライダー0.2の位置で、0.3以上はひたすら
    // 悪化するだけの領域になった。1.0 にすると最良点が0.5〜0.6付近に来て、
    // スライダーの可動域が意味を持つ。
    // 上限1.0での実測（ノイズσ0.0173の画像、元画像との誤差）:
    //   denoise: 0.0    0.1    0.2    0.3    0.5    0.8
    //   誤差   : .0173  .0160  .0149  .0140  .0131  .0136
    struct ActiveLayer {
        int index;
        double sharpen;
        double threshold;
    };
    std::vector<ActiveLayer> active;
    for (int j = 0; j < layers_; ++j) {
        const WaveletLayerParams& p = params[static_cast<std::size_t>(j)];
        if (p.sharpen == 1.0 && p.denoise == 0.0) continue;
        active.push_back({j, p.sharpen, p.denoise * 1.0 * noise_[static_cast<std::size_t>(j)]});
    }
    const int width = width_;
    const std::size_t count = active.size();
    for (int c = 0; c < channels_; ++c) {
        const std::size_t offset = static_cast<std::size_t>(c) * plane_;
        detail::parallel_rows(height_, [&](int y0, int y1) {
            std::vector<const float*> rows(count);
            for (int y = y0; y < y1; ++y) {
                float* d = out.row(c, y);
                const std::size_t base = offset + static_cast<std::size_t>(y) * width;
                const float* src = base_.data() + base;
                for (std::size_t k = 0; k < count; ++k) {
                    rows[k] = detail_[static_cast<std::size_t>(active[k].index)].data() + base;
                }
                for (int x = 0; x < width; ++x) {
                    double acc = src[x];
                    for (std::size_t k = 0; k < count; ++k) {
                        const double w = rows[k][x];
                        double v = w;
                        const double threshold = active[k].threshold;
                        if (threshold > 0.0) {
                            // soft-threshold: しきい値ぶん0へ寄せ、超えない分は0にする。
                            // hard-thresholdと違って不連続を作らないので、
                            // 残った構造に段差が出ない。
                            if (v > threshold) {
                                v -= threshold;
                            } else if (v < -threshold) {
                                v += threshold;
                            } else {
                                v = 0.0;
                            }
                        }
                        acc += active[k].sharpen * v - w;
                    }
                    d[x] = static_cast<float>(acc);
                }
            }
        });
    }
    out.invalidate_luma();
}

void stretch_histogram(const FrameBuffer& src, double black, double white, double gamma,
                       FrameBuffer& out) {
    if (src.empty()) throw std::invalid_argument("ヒストグラムストレッチ: 空のフレームです");
    if (!(white > black)) {
        throw std::invalid_argument("ヒストグラムストレッチ: 白点は黒点より大きい必要があります");
    }
    if (!(gamma > 0.0)) {
        throw std::invalid_argument("ヒストグラムストレッチ: ガンマは正の値です");
    }

    if (out.width() != src.width() || out.height() != src.height() ||
        out.channels() != src.channels()) {
        out.reset(src.width(), src.height(), src.channels());
    }

    const double scale = 1.0 / (white - black);
    const double inv_gamma = 1.0 / gamma;
    const bool linear = std::fabs(gamma - 1.0) < 1e-12;

    // 画素ごとに独立（src と out が同じでもよい）。
    const int width = src.width();
    for (int c = 0; c < src.channels(); ++c) {
        detail::parallel_rows(src.height(), [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const float* s = src.row(c, y);
                float* d = out.row(c, y);
                for (int x = 0; x < width; ++x) {
                    double v = (static_cast<double>(s[x]) - black) * scale;
                    if (v < 0.0) v = 0.0;
                    if (v > 1.0) v = 1.0;
                    if (!linear) v = std::pow(v, inv_gamma);
                    d[x] = static_cast<float>(v);
                }
            }
        });
    }
    out.invalidate_luma();
    out.set_source_bit_depth(src.source_bit_depth());
}

}  // namespace stackcore
