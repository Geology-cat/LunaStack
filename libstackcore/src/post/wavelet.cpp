#include "stackcore/wavelet.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace stackcore {
namespace {

// B3スプライン 5タップ [1,4,6,4,1]/16。
const double kB3[5] = {1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0};

int clamp_index(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// レイヤー j の穴あき畳み込み。タップ間隔は 2^j。
// 分離可能なので横→縦の2パスで行う。
void atrous_convolve(const float* src, float* dst, float* scratch, int w, int h, int step) {
    for (int y = 0; y < h; ++y) {
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
    for (int y = 0; y < h; ++y) {
        float* d = dst + static_cast<std::size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            double acc = 0.0;
            for (int k = 0; k < 5; ++k) {
                const int sy = clamp_index(y + (k - 2) * step, 0, h - 1);
                acc += kB3[k] * scratch[static_cast<std::size_t>(sy) * w + x];
            }
            d[x] = static_cast<float>(acc);
        }
    }
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
    residual_.assign(total, 0.0f);
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

    for (int j = 0; j < layers; ++j) {
        const int step = 1 << j;
        for (int c = 0; c < channels_; ++c) {
            const float* cur = current.data() + static_cast<std::size_t>(c) * plane_;
            float* nx = next.data() + static_cast<std::size_t>(c) * plane_;
            atrous_convolve(cur, nx, scratch.data(), width_, height_, step);

            // w_j = c_{j-1} - c_j
            float* w = detail_[static_cast<std::size_t>(j)].data() +
                       static_cast<std::size_t>(c) * plane_;
            for (std::size_t i = 0; i < plane_; ++i) w[i] = cur[i] - nx[i];
        }
        current.swap(next);
    }
    residual_ = current;  // c_J

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

    if (out.width() != width_ || out.height() != height_ || out.channels() != channels_) {
        out.reset(width_, height_, channels_);
    }

    for (int c = 0; c < channels_; ++c) {
        const float* res = residual_.data() + static_cast<std::size_t>(c) * plane_;
        for (int y = 0; y < height_; ++y) {
            float* d = out.row(c, y);
            const std::size_t base = static_cast<std::size_t>(y) * width_;
            for (int x = 0; x < width_; ++x) d[x] = res[base + x];
        }
    }

    for (int j = 0; j < layers_; ++j) {
        const WaveletLayerParams& p = params[static_cast<std::size_t>(j)];
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
        const double threshold = p.denoise * 1.0 * noise_[static_cast<std::size_t>(j)];

        for (int c = 0; c < channels_; ++c) {
            const float* w = detail_[static_cast<std::size_t>(j)].data() +
                             static_cast<std::size_t>(c) * plane_;
            for (int y = 0; y < height_; ++y) {
                float* d = out.row(c, y);
                const std::size_t base = static_cast<std::size_t>(y) * width_;
                for (int x = 0; x < width_; ++x) {
                    double v = w[base + x];
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
                    d[x] += static_cast<float>(p.sharpen * v);
                }
            }
        }
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

    for (int c = 0; c < src.channels(); ++c) {
        for (int y = 0; y < src.height(); ++y) {
            const float* s = src.row(c, y);
            float* d = out.row(c, y);
            for (int x = 0; x < src.width(); ++x) {
                double v = (static_cast<double>(s[x]) - black) * scale;
                if (v < 0.0) v = 0.0;
                if (v > 1.0) v = 1.0;
                if (!linear) v = std::pow(v, inv_gamma);
                d[x] = static_cast<float>(v);
            }
        }
    }
    out.invalidate_luma();
    out.set_source_bit_depth(src.source_bit_depth());
}

}  // namespace stackcore
