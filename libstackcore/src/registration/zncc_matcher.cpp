#include "stackcore/zncc_matcher.hpp"

#include <Accelerate/Accelerate.h>

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

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

}  // namespace

void quadratic_subpixel(const double values[3][3], double& out_dx, double& out_dy) {
    // 2次曲面 f(x,y) = a x² + b y² + c xy + d x + e y + g を3x3にあてはめる。
    // 等間隔の3x3では最小二乗解が有限差分の形になる。
    const double c00 = values[0][0], c01 = values[0][1], c02 = values[0][2];
    const double c10 = values[1][0], c11 = values[1][1], c12 = values[1][2];
    const double c20 = values[2][0], c21 = values[2][1], c22 = values[2][2];

    const double dx = (c12 - c10) * 0.5;
    const double dy = (c21 - c01) * 0.5;
    const double dxx = c12 - 2.0 * c11 + c10;
    const double dyy = c21 - 2.0 * c11 + c01;
    const double dxy = (c22 - c20 - c02 + c00) * 0.25;

    const double det = dxx * dyy - dxy * dxy;

    // det が0に近いと解が発散する。平坦な相関面（模様のない領域）や
    // 稜線状のピーク（一方向にしか特徴がない領域）で起きる。
    // その場合は各軸独立の1次元放物線に落とす。
    if (std::fabs(det) < 1e-12) {
        out_dx = std::fabs(dxx) > 1e-12 ? -dx / dxx : 0.0;
        out_dy = std::fabs(dyy) > 1e-12 ? -dy / dyy : 0.0;
    } else {
        out_dx = -(dyy * dx - dxy * dy) / det;
        out_dy = -(dxx * dy - dxy * dx) / det;
    }

    // 3x3の外へ出る補正は外挿であり信用できない。整数ピークの隣までに丸める。
    if (out_dx > 1.0) out_dx = 1.0;
    if (out_dx < -1.0) out_dx = -1.0;
    if (out_dy > 1.0) out_dy = 1.0;
    if (out_dy < -1.0) out_dy = -1.0;
    if (!(out_dx == out_dx)) out_dx = 0.0;  // NaN 対策
    if (!(out_dy == out_dy)) out_dy = 0.0;
}

struct ZnccMatcher::Impl {
    int tmpl_size = 0;
    int radius = 0;
    int search_size = 0;  // tmpl_size + 2*radius
    int n = 0;            // FFTのパディングサイズ（正方の2の冪）
    vDSP_Length log2n = 0;
    FFTSetup setup = nullptr;

    // テンプレート（ゼロ平均化済み）のFFT。フレームごとに再計算しない。
    std::vector<float> tmpl_re, tmpl_im;
    bool has_template = false;
    double tmpl_norm = 0.0;  // sqrt(Σ(t - mean)²)

    std::vector<float> cur_re, cur_im;
    std::vector<double> integral, integral_sq;

    ~Impl() {
        if (setup) vDSP_destroy_fftsetup(setup);
    }

    // 探索画像の積分画像（和と2乗和）を作る。
    // 候補変位ごとの窓の平均と分散をO(1)で引くために使う。
    void build_integrals(const float* src, std::size_t stride) {
        const int w = search_size, h = search_size;
        const std::size_t iw = static_cast<std::size_t>(w) + 1;
        integral.assign(iw * (h + 1), 0.0);
        integral_sq.assign(iw * (h + 1), 0.0);
        for (int y = 0; y < h; ++y) {
            double row = 0.0, row_sq = 0.0;
            const float* s = src + static_cast<std::size_t>(y) * stride;
            for (int x = 0; x < w; ++x) {
                const double v = s[x];
                row += v;
                row_sq += v * v;
                integral[static_cast<std::size_t>(y + 1) * iw + (x + 1)] =
                    integral[static_cast<std::size_t>(y) * iw + (x + 1)] + row;
                integral_sq[static_cast<std::size_t>(y + 1) * iw + (x + 1)] =
                    integral_sq[static_cast<std::size_t>(y) * iw + (x + 1)] + row_sq;
            }
        }
    }

    double rect(const std::vector<double>& in, int x0, int y0, int x1, int y1) const {
        const std::size_t iw = static_cast<std::size_t>(search_size) + 1;
        return in[static_cast<std::size_t>(y1) * iw + x1] -
               in[static_cast<std::size_t>(y0) * iw + x1] -
               in[static_cast<std::size_t>(y1) * iw + x0] +
               in[static_cast<std::size_t>(y0) * iw + x0];
    }
};

ZnccMatcher::ZnccMatcher(int template_size, int search_radius) : impl_(new Impl()) {
    if (template_size < 8 || search_radius < 1) {
        throw std::invalid_argument("ZnccMatcher: テンプレートサイズまたは探索半径が不正です");
    }
    impl_->tmpl_size = template_size;
    impl_->radius = search_radius;
    impl_->search_size = template_size + 2 * search_radius;
    impl_->n = next_pow2(impl_->search_size);
    impl_->log2n = log2_of(impl_->n);
    impl_->setup = vDSP_create_fftsetup(impl_->log2n, FFT_RADIX2);
    if (!impl_->setup) {
        throw std::runtime_error("ZnccMatcher: vDSPのFFTセットアップ確保に失敗しました");
    }
    const std::size_t total = static_cast<std::size_t>(impl_->n) * impl_->n;
    impl_->tmpl_re.assign(total, 0.0f);
    impl_->tmpl_im.assign(total, 0.0f);
    impl_->cur_re.assign(total, 0.0f);
    impl_->cur_im.assign(total, 0.0f);
}

ZnccMatcher::~ZnccMatcher() = default;

int ZnccMatcher::template_size() const noexcept { return impl_->tmpl_size; }
int ZnccMatcher::search_radius() const noexcept { return impl_->radius; }
int ZnccMatcher::padded_size() const noexcept { return impl_->n; }

bool ZnccMatcher::set_template(const float* tmpl, std::size_t stride) {
    const int t = impl_->tmpl_size;
    const int n = impl_->n;

    double sum = 0.0, sum_sq = 0.0;
    for (int y = 0; y < t; ++y) {
        const float* row = tmpl + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < t; ++x) {
            sum += row[x];
            sum_sq += static_cast<double>(row[x]) * row[x];
        }
    }
    const double count = static_cast<double>(t) * t;
    const double mean = sum / count;
    const double var = sum_sq - mean * sum;  // Σ(v-mean)²
    if (!(var > 1e-20)) {
        impl_->has_template = false;
        return false;  // 一様な領域。相関を取っても意味がない。
    }
    impl_->tmpl_norm = std::sqrt(var);

    std::memset(impl_->tmpl_re.data(), 0, impl_->tmpl_re.size() * sizeof(float));
    std::memset(impl_->tmpl_im.data(), 0, impl_->tmpl_im.size() * sizeof(float));
    for (int y = 0; y < t; ++y) {
        const float* row = tmpl + static_cast<std::size_t>(y) * stride;
        float* d = impl_->tmpl_re.data() + static_cast<std::size_t>(y) * n;
        for (int x = 0; x < t; ++x) d[x] = static_cast<float>(row[x] - mean);
    }

    DSPSplitComplex sc{impl_->tmpl_re.data(), impl_->tmpl_im.data()};
    vDSP_fft2d_zip(impl_->setup, &sc, 1, 0, impl_->log2n, impl_->log2n, FFT_FORWARD);
    impl_->has_template = true;
    return true;
}

MatchResult ZnccMatcher::match(const float* search, std::size_t stride) {
    if (!impl_->has_template) {
        throw std::logic_error("ZnccMatcher: テンプレートが設定されていません");
    }
    const int t = impl_->tmpl_size;
    const int n = impl_->n;
    const int r = impl_->radius;
    const int s = impl_->search_size;

    // 分子: テンプレート（ゼロ平均）と探索画像の相互相関。
    // ゼロ平均のテンプレートと掛けるので、探索窓側の平均は自動的に打ち消える
    // （Σ(t-mt)(f-mf) = Σ(t-mt)f）。よって探索側はゼロ平均化しなくてよい。
    std::memset(impl_->cur_re.data(), 0, impl_->cur_re.size() * sizeof(float));
    std::memset(impl_->cur_im.data(), 0, impl_->cur_im.size() * sizeof(float));
    for (int y = 0; y < s; ++y) {
        const float* row = search + static_cast<std::size_t>(y) * stride;
        float* d = impl_->cur_re.data() + static_cast<std::size_t>(y) * n;
        for (int x = 0; x < s; ++x) d[x] = row[x];
    }

    DSPSplitComplex sc{impl_->cur_re.data(), impl_->cur_im.data()};
    vDSP_fft2d_zip(impl_->setup, &sc, 1, 0, impl_->log2n, impl_->log2n, FFT_FORWARD);

    // conj(T) * F の逆変換が相互相関になる。
    const float* tr = impl_->tmpl_re.data();
    const float* ti = impl_->tmpl_im.data();
    float* cr = impl_->cur_re.data();
    float* ci = impl_->cur_im.data();
    const std::size_t total = static_cast<std::size_t>(n) * n;
    for (std::size_t i = 0; i < total; ++i) {
        const float re = tr[i] * cr[i] + ti[i] * ci[i];
        const float im = tr[i] * ci[i] - ti[i] * cr[i];
        cr[i] = re;
        ci[i] = im;
    }
    vDSP_fft2d_zip(impl_->setup, &sc, 1, 0, impl_->log2n, impl_->log2n, FFT_INVERSE);

    const double scale = 1.0 / (static_cast<double>(n) * n);
    impl_->build_integrals(search, stride);

    // 候補変位 (ox, oy) は探索画像内でのテンプレート左上の位置。
    // 0..2r の範囲を動き、中心 (r, r) が変位ゼロに対応する。
    const double count = static_cast<double>(t) * t;
    const int candidates = 2 * r + 1;
    std::vector<double> zncc(static_cast<std::size_t>(candidates) * candidates, -2.0);

    for (int oy = 0; oy < candidates; ++oy) {
        for (int ox = 0; ox < candidates; ++ox) {
            const double numer =
                static_cast<double>(cr[static_cast<std::size_t>(oy) * n + ox]) * scale;
            const double win_sum = impl_->rect(impl_->integral, ox, oy, ox + t, oy + t);
            const double win_sq = impl_->rect(impl_->integral_sq, ox, oy, ox + t, oy + t);
            const double win_var = win_sq - win_sum * win_sum / count;
            if (!(win_var > 1e-20)) continue;  // 一様な窓。相関は定義できない。
            zncc[static_cast<std::size_t>(oy) * candidates + ox] =
                numer / (impl_->tmpl_norm * std::sqrt(win_var));
        }
    }

    int best_x = r, best_y = r;
    double best = -2.0;
    for (int oy = 0; oy < candidates; ++oy) {
        for (int ox = 0; ox < candidates; ++ox) {
            const double v = zncc[static_cast<std::size_t>(oy) * candidates + ox];
            if (v > best) {
                best = v;
                best_x = ox;
                best_y = oy;
            }
        }
    }

    MatchResult out;
    out.score_at_zero = zncc[static_cast<std::size_t>(r) * candidates + r];
    out.peak_dx = best_x - r;
    out.peak_dy = best_y - r;
    out.score = best;
    out.at_search_limit =
        best_x == 0 || best_y == 0 || best_x == candidates - 1 || best_y == candidates - 1;

    if (out.at_search_limit) {
        // 縁ではサブピクセル推定に必要な3x3が揃わない。整数値のまま返す。
        out.dx = out.peak_dx;
        out.dy = out.peak_dy;
        return out;
    }

    double nb[3][3];
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            nb[j + 1][i + 1] =
                zncc[static_cast<std::size_t>(best_y + j) * candidates + (best_x + i)];
        }
    }
    double sub_dx = 0.0, sub_dy = 0.0;
    quadratic_subpixel(nb, sub_dx, sub_dy);
    out.dx = out.peak_dx + sub_dx;
    out.dy = out.peak_dy + sub_dy;
    return out;
}

}  // namespace stackcore
