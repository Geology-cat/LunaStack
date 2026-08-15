#include "stackcore/phase_correlate.hpp"

#include <Accelerate/Accelerate.h>

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "stackcore/frame_buffer.hpp"

namespace stackcore {
namespace {

int next_pow2(int v) {
    int n = 1;
    while (n < v) n <<= 1;
    return n;
}

vDSP_Length log2_of(int v) {
    vDSP_Length k = 0;
    while ((1 << k) < v) ++k;
    return k;
}

// Hann窓。FFTは画像が周期的に繋がっていると仮定するので、窓を掛けずに
// 端の不連続を残すと、そこから出る強い偽の周波数成分が相関面を汚す。
std::vector<float> hann(int n) {
    std::vector<float> w(static_cast<std::size_t>(n));
    if (n == 1) {
        w[0] = 1.0f;
        return w;
    }
    for (int i = 0; i < n; ++i) {
        w[static_cast<std::size_t>(i)] =
            0.5f * (1.0f - std::cos(2.0 * M_PI * i / (n - 1)));
    }
    return w;
}

}  // namespace

struct PhaseCorrelator::Impl {
    PhaseCorrelateSettings settings;
    int n = 0;
    vDSP_Length log2n = 0;
    FFTSetup setup = nullptr;
    std::vector<float> lowpass;  // 周波数ごとの重み。空なら無効。

    AlignedFloats ref_re, ref_im;
    AlignedFloats cur_re, cur_im;

    std::vector<float> win_x, win_y;
    int win_w = 0, win_h = 0;
    bool has_reference = false;

    ~Impl() {
        if (setup) vDSP_destroy_fftsetup(setup);
    }

    void ensure_window(int w, int h) {
        if (win_w == w && win_h == h) return;
        win_x = hann(w);
        win_y = hann(h);
        win_w = w;
        win_h = h;
    }

    // 画像に窓を掛けてゼロ詰めのn×n複素バッファへ載せ、前方FFTを掛ける。
    void load_and_transform(const float* src, int w, int h, std::size_t stride,
                            AlignedFloats& re, AlignedFloats& im) {
        if (w > n || h > n) {
            throw std::invalid_argument("PhaseCorrelator: 画像がパディングサイズを超えています");
        }
        ensure_window(w, h);

        const std::size_t total = static_cast<std::size_t>(n) * n;
        if (re.size() != total) re.reset(total);
        if (im.size() != total) im.reset(total);

        float* rp = re.data();
        float* ip = im.data();
        std::memset(rp, 0, total * sizeof(float));
        std::memset(ip, 0, total * sizeof(float));

        for (int y = 0; y < h; ++y) {
            const float* s = src + static_cast<std::size_t>(y) * stride;
            float* d = rp + static_cast<std::size_t>(y) * n;
            const float wy = win_y[static_cast<std::size_t>(y)];
            for (int x = 0; x < w; ++x) {
                d[x] = s[x] * win_x[static_cast<std::size_t>(x)] * wy;
            }
        }

        DSPSplitComplex sc{rp, ip};
        vDSP_fft2d_zip(setup, &sc, 1, 0, log2n, log2n, FFT_FORWARD);
    }
};

PhaseCorrelator::PhaseCorrelator(int width, int height,
                                 const PhaseCorrelateSettings& settings)
    : impl_(new Impl()) {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("PhaseCorrelator: 幅・高さは正の値である必要があります");
    }
    impl_->settings = settings;
    impl_->n = next_pow2(width > height ? width : height);
    impl_->log2n = log2_of(impl_->n);
    impl_->setup = vDSP_create_fftsetup(impl_->log2n, FFT_RADIX2);
    if (!impl_->setup) {
        throw std::runtime_error("PhaseCorrelator: vDSPのFFTセットアップ確保に失敗しました");
    }

    if (settings.lowpass_sigma > 0.0) {
        const int n = impl_->n;
        impl_->lowpass.resize(static_cast<std::size_t>(n) * n);
        const double half = n / 2.0;
        const double denom = 2.0 * settings.lowpass_sigma * settings.lowpass_sigma;
        for (int v = 0; v < n; ++v) {
            // 周波数の添字は [0, n) だが、n/2 以上は負の周波数を表す。
            const double fy = (v >= n / 2 ? v - n : v) / half;
            for (int u = 0; u < n; ++u) {
                const double fx = (u >= n / 2 ? u - n : u) / half;
                const double r2 = fx * fx + fy * fy;
                impl_->lowpass[static_cast<std::size_t>(v) * n + u] =
                    static_cast<float>(std::exp(-r2 / denom));
            }
        }
    }
}

PhaseCorrelator::~PhaseCorrelator() = default;

int PhaseCorrelator::padded_size() const noexcept { return impl_->n; }

int PhaseCorrelator::max_shift() const noexcept { return impl_->n / 2; }

void PhaseCorrelator::set_reference(const float* ref, int width, int height,
                                    std::size_t stride) {
    impl_->load_and_transform(ref, width, height, stride, impl_->ref_re, impl_->ref_im);
    impl_->has_reference = true;
}

CorrelationPeak PhaseCorrelator::correlate(const float* img, int width, int height,
                                           std::size_t stride) {
    if (!impl_->has_reference) {
        throw std::logic_error("PhaseCorrelator: 参照画像が設定されていません");
    }
    impl_->load_and_transform(img, width, height, stride, impl_->cur_re, impl_->cur_im);

    const int n = impl_->n;
    const std::size_t total = static_cast<std::size_t>(n) * n;
    const float* ar = impl_->ref_re.data();
    const float* ai = impl_->ref_im.data();
    float* br = impl_->cur_re.data();
    float* bi = impl_->cur_im.data();

    // 相互パワースペクトル R = A・conj(B) / |A・conj(B)|^α。
    // α=1 なら振幅を完全に捨てる純粋な位相相関、α=0 なら通常の相互相関。
    // 中間の値にすることで、明るさの違いへの強さを保ったまま、
    // ノイズしかない高周波を増幅しすぎないようにする（settings.whitening 参照）。
    const double alpha = impl_->settings.whitening;
    const bool use_lowpass = !impl_->lowpass.empty();
    for (std::size_t i = 0; i < total; ++i) {
        const float cr = ar[i] * br[i] + ai[i] * bi[i];
        const float ci = ai[i] * br[i] - ar[i] * bi[i];
        const float mag = std::sqrt(cr * cr + ci * ci);
        // 振幅0の成分（全くエネルギーのない周波数）は位相が定義できない。
        // 0除算を避けるため寄与させない。
        float scale;
        if (mag > 1e-20f) {
            scale = alpha == 1.0 ? 1.0f / mag
                                 : static_cast<float>(std::pow(static_cast<double>(mag), -alpha));
        } else {
            scale = 0.0f;
        }
        if (use_lowpass) scale *= impl_->lowpass[i];
        br[i] = cr * scale;
        bi[i] = ci * scale;
    }

    DSPSplitComplex sc{br, bi};
    vDSP_fft2d_zip(impl_->setup, &sc, 1, 0, impl_->log2n, impl_->log2n, FFT_INVERSE);

    // 逆FFTは正規化されないので n*n で割る。
    const double scale = 1.0 / (static_cast<double>(n) * n);

    // ピーク探索。同値のときは先に見つけた方を採る（< ではなく > で比較）ことで
    // 実行ごとに結果が変わらないようにする。
    std::size_t best = 0;
    double best_val = -1.0;
    for (std::size_t i = 0; i < total; ++i) {
        const double v = static_cast<double>(br[i]) * scale;
        if (v > best_val) {
            best_val = v;
            best = i;
        }
    }

    const int py = static_cast<int>(best / static_cast<std::size_t>(n));
    const int px = static_cast<int>(best % static_cast<std::size_t>(n));

    // ピーク近傍を除いた相関面の平均と標準偏差。
    // 除外しないとピーク自身が統計量を押し上げ、鋭いピークほど
    // 比が下がるという逆転が起きる。
    constexpr int kExclusionRadius = 5;
    double sum = 0.0, sum_sq = 0.0;
    std::size_t count = 0;
    for (int y = 0; y < n; ++y) {
        // 循環相関なので、除外範囲も端で巻き戻して判定する。
        int ody = y - py;
        if (ody > n / 2) ody -= n;
        if (ody < -n / 2) ody += n;
        if (ody < 0) ody = -ody;
        if (ody <= kExclusionRadius) {
            // この行はピーク近傍の列だけを除く
            for (int x = 0; x < n; ++x) {
                int odx = x - px;
                if (odx > n / 2) odx -= n;
                if (odx < -n / 2) odx += n;
                if (odx < 0) odx = -odx;
                if (odx <= kExclusionRadius) continue;
                const double v = static_cast<double>(br[static_cast<std::size_t>(y) * n + x]) * scale;
                sum += v;
                sum_sq += v * v;
                ++count;
            }
        } else {
            for (int x = 0; x < n; ++x) {
                const double v = static_cast<double>(br[static_cast<std::size_t>(y) * n + x]) * scale;
                sum += v;
                sum_sq += v * v;
                ++count;
            }
        }
    }

    CorrelationPeak out;
    // [0, n) を [-n/2, n/2) に折り返す。
    out.dx = px >= n / 2 ? px - n : px;
    out.dy = py >= n / 2 ? py - n : py;
    out.peak = best_val;

    if (count > 1) {
        const double mean = sum / static_cast<double>(count);
        const double var = sum_sq / static_cast<double>(count) - mean * mean;
        const double sd = var > 0.0 ? std::sqrt(var) : 0.0;
        out.peak_sidelobe_ratio = sd > 1e-30 ? (best_val - mean) / sd : 0.0;
    }
    return out;
}

}  // namespace stackcore
