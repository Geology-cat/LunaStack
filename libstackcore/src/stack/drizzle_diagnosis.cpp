#include "stackcore/drizzle_diagnosis.hpp"

#include <Accelerate/Accelerate.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "stackcore/finishing.hpp"

namespace stackcore {
namespace {

// 輝度（Rec.709）。1chならそのまま。
float luma_at(const FrameBuffer& f, int x, int y) {
    if (f.channels() >= 3) {
        return 0.2126f * f.row(0, y)[x] + 0.7152f * f.row(1, y)[x] + 0.0722f * f.row(2, y)[x];
    }
    return f.row(0, y)[x];
}

// 像の細かさから見た倍率の上限（折り返しの先は測れないので、これ以上は言わない）。
constexpr double kMaxCutoff = 2.0;

// 背景の空のノイズ（σ）。対象を囲む矩形を余白ぶん広げた外側で、横に隣り合う画素の差の
// 中央絶対偏差から求める（なだらかな明るさの傾きには左右されない）。
// 空が十分に写っていなければ（月面・太陽の全面など）false。
bool background_noise(const FrameBuffer& ref, int bx, int by, int bw, int bh, double& sigma) {
    const int W = ref.width(), H = ref.height();
    const int margin = std::max(8, std::max(bw, bh) / 10);
    const int x0 = bx - margin, y0 = by - margin, x1 = bx + bw + margin, y1 = by + bh + margin;
    const auto sky = [&](int x, int y) { return x < x0 || x >= x1 || y < y0 || y >= y1; };
    std::vector<float> diff;
    for (int y = 0; y < H; y += (H > 1024 ? 2 : 1)) {
        for (int x = 0; x + 1 < W; ++x) {
            if (sky(x, y) && sky(x + 1, y)) diff.push_back(luma_at(ref, x + 1, y) - luma_at(ref, x, y));
        }
    }
    if (diff.size() < 4000) return false;
    const std::ptrdiff_t mid = static_cast<std::ptrdiff_t>(diff.size() / 2);
    std::nth_element(diff.begin(), diff.begin() + mid, diff.end());
    const float med = diff[static_cast<std::size_t>(mid)];
    for (float& v : diff) v = std::fabs(v - med);
    std::nth_element(diff.begin(), diff.begin() + mid, diff.end());
    sigma = 1.4826 * diff[static_cast<std::size_t>(mid)] / std::sqrt(2.0);
    return sigma > 0.0;
}

// 1. 像の細かさ: 参照画像の、対象を含む正方形の動径パワースペクトルから、信号がノイズの2倍まで
// 落ちる周波数を、ナイキスト周波数を1として求める。
//
// ノイズの床は背景の空のσから決める（白色ノイズなら σ²Σw²）。1枚の画像の細かい周期だけを見ても、
// 細部とノイズは見分けられない（粗く撮れた像ほど、細かい周期まで信号がある）ためである。
// このときは斜めの最も細かい周期（ナイキストの1.41倍）まで測り、そこでも落ちきらなければ外挿する。
// 明るい対象ではショットノイズのぶん、空よりも像の上のほうがノイズが多く、見積もりはやや甘くなる。
//
// 空が写っていなければ、2次元スペクトルの四隅（1.2倍より外）の中央値を床とし、ナイキストまでを測る。
// 粗く撮れた像では四隅にも信号が残るので、倍率は控えめ（小さめ）に出る。
bool sampling_cutoff(const FrameBuffer& ref, double& cutoff, bool& extrapolated, bool& from_background) {
    const int W = ref.width(), H = ref.height();
    int bx = 0, by = 0, bw = W, bh = H;
    detect_object_bounds(ref, 0, bx, by, bw, bh);
    double sigma = 0.0;
    from_background = background_noise(ref, bx, by, bw, bh, sigma);
    // 正方形の一辺: 2の累乗で、画像に収まり、対象の大きさ程度（64〜1024）。
    const int limit = std::min(std::min(W, H), 1024);
    int want = std::max(64, std::max(bw, bh));
    int n = 64;
    while (n * 2 <= limit && n < want) n *= 2;
    if (n > limit) return false;
    int x0 = bx + bw / 2 - n / 2, y0 = by + bh / 2 - n / 2;
    x0 = std::max(0, std::min(x0, W - n));
    y0 = std::max(0, std::min(y0, H - n));

    int log2n = 0;
    while ((1 << log2n) < n) ++log2n;
    std::vector<float> re(static_cast<std::size_t>(n) * n), im(static_cast<std::size_t>(n) * n, 0.0f);
    std::vector<float> w1(static_cast<std::size_t>(n));
    double w2 = 0.0;
    for (int i = 0; i < n; ++i) {
        w1[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * M_PI * i / n));
        w2 += static_cast<double>(w1[static_cast<std::size_t>(i)]) * w1[static_cast<std::size_t>(i)];
    }
    double mean = 0.0;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) mean += luma_at(ref, x0 + x, y0 + y);
    }
    mean /= static_cast<double>(n) * n;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            re[static_cast<std::size_t>(y) * n + x] =
                static_cast<float>((luma_at(ref, x0 + x, y0 + y) - mean) * w1[static_cast<std::size_t>(x)] *
                                   w1[static_cast<std::size_t>(y)]);
        }
    }
    FFTSetup setup = vDSP_create_fftsetup(static_cast<vDSP_Length>(log2n), kFFTRadix2);
    if (!setup) return false;
    DSPSplitComplex sc{re.data(), im.data()};
    vDSP_fft2d_zip(setup, &sc, 1, 0, static_cast<vDSP_Length>(log2n), static_cast<vDSP_Length>(log2n), FFT_FORWARD);
    vDSP_destroy_fftsetup(setup);

    // 動径の平均（半径はナイキストを1とし、1/64 刻み）。空のσがあれば 1.4 まで、無ければ 1 まで。
    const double r_max = from_background ? 1.4 : 1.0;
    const int bins = static_cast<int>(r_max * 64 + 0.5);
    std::vector<double> sum(static_cast<std::size_t>(bins), 0.0), count(static_cast<std::size_t>(bins), 0.0);
    std::vector<double> corner;
    const double half = n / 2.0;
    for (int ky = 0; ky < n; ++ky) {
        const int fy = ky < n / 2 ? ky : ky - n;
        for (int kx = 0; kx < n; ++kx) {
            const int fx = kx < n / 2 ? kx : kx - n;
            const double r = std::sqrt(static_cast<double>(fx) * fx + static_cast<double>(fy) * fy) / half;
            const std::size_t i = static_cast<std::size_t>(ky) * n + kx;
            const double p = static_cast<double>(re[i]) * re[i] + static_cast<double>(im[i]) * im[i];
            if (r < r_max) {
                const int b = std::min(bins - 1, static_cast<int>(r * 64));
                sum[static_cast<std::size_t>(b)] += p;
                count[static_cast<std::size_t>(b)] += 1.0;
            }
            if (!from_background && r >= 1.2) corner.push_back(p);
        }
    }
    double floor_power = 0.0;
    if (from_background) {
        floor_power = sigma * sigma * w2 * w2;
    } else {
        if (corner.size() < 16) return false;
        std::nth_element(corner.begin(), corner.begin() + static_cast<std::ptrdiff_t>(corner.size() / 2),
                         corner.end());
        floor_power = corner[corner.size() / 2];
    }
    const double floor_db = 10.0 * std::log10(std::max(1e-30, floor_power));
    std::vector<double> level(static_cast<std::size_t>(bins), floor_db);
    for (int b = 0; b < bins; ++b) {
        if (count[static_cast<std::size_t>(b)] > 0) {
            level[static_cast<std::size_t>(b)] =
                10.0 * std::log10(std::max(1e-30, sum[static_cast<std::size_t>(b)] / count[static_cast<std::size_t>(b)]));
        }
    }
    // 3段の移動平均でならす。
    std::vector<double> smooth(static_cast<std::size_t>(bins));
    for (int b = 0; b < bins; ++b) {
        double s = 0.0;
        int c = 0;
        for (int k = std::max(0, b - 1); k <= std::min(bins - 1, b + 1); ++k) {
            s += level[static_cast<std::size_t>(k)];
            ++c;
        }
        smooth[static_cast<std::size_t>(b)] = s / c;
    }
    const double threshold = floor_db + 3.0;  // 信号がノイズの2倍
    for (int b = 6; b + 3 <= bins; ++b) {
        bool below = true;
        for (int k = b; k < b + 3; ++k) below = below && smooth[static_cast<std::size_t>(k)] < threshold;
        if (below) {
            cutoff = (b + 0.5) / 64.0;
            extrapolated = false;
            return true;
        }
    }
    // 測った範囲で落ちきらない。後半（r_max の 0.6〜1 倍）の傾き（dB / ナイキスト）で外挿する。
    // ナイキストより細かい周期は折り返して重なるので、その先は1枚の画像からは見分けられない。
    // 外挿は 2 で頭打ちにする（「少なくとも r_max、たぶんそれ以上」の意味）。
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int m = 0;
    for (int b = static_cast<int>(bins * 0.6); b < bins; ++b) {
        const double x = (b + 0.5) / 64.0, y = smooth[static_cast<std::size_t>(b)];
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
        ++m;
    }
    const double slope = (m * sxy - sx * sy) / std::max(1e-12, m * sxx - sx * sx);
    const double at_end = smooth[static_cast<std::size_t>(bins - 1)];
    extrapolated = true;
    cutoff = slope >= -1.0 ? kMaxCutoff : r_max + (at_end - threshold) / (-slope);
    cutoff = std::min(kMaxCutoff, std::max(r_max, cutoff));
    return true;
}

double snap_scale(double v) {
    if (v >= 2.0) return 2.0;
    if (v >= 1.5) return 1.5;
    return 1.0;
}

}  // namespace

DrizzleDiagnosis diagnose_drizzle(const AnalysisData& analysis, const FrameBuffer& reference,
                                  const MapStackSettings& settings) {
    DrizzleDiagnosis d;
    const std::size_t ap_count = analysis.points.size();
    const std::size_t frame_count = analysis.analyzed_indices.size();
    if (ap_count == 0 || frame_count == 0 || analysis.matrix.size() != ap_count * frame_count ||
        reference.empty() || reference.width() < 64 || reference.height() < 64) {
        d.insufficient = true;
        return d;
    }

    // 1. 像の細かさ
    if (!sampling_cutoff(reference, d.cutoff_ratio, d.cutoff_extrapolated, d.floor_from_background)) {
        d.insufficient = true;
        return d;
    }
    d.limit_sampling = std::max(1.0, std::min(kMaxCutoff, d.cutoff_ratio));

    // 3. 枚数（スタックと同じ数え方）
    int keep = settings.ap_top_count > 0
                   ? settings.ap_top_count
                   : static_cast<int>(frame_count * settings.ap_top_percent / 100.0 + 0.5);
    keep = std::max(1, std::min(keep, static_cast<int>(frame_count)));
    d.frames_per_ap = keep;
    // 拡大率 s では1枚の寄与が s² の画素に分かれる。目安として1画素あたり5枚ぶん以上ほしい。
    d.limit_frames = std::max(1.0, std::min(3.0, std::sqrt(keep / 5.0)));

    // 2. 位置のばらつき（スタックと同じく、APごとに品質の上位 keep 枚）
    std::vector<double> cov2, cov3;
    std::vector<std::size_t> order(frame_count);
    for (std::size_t a = 0; a < ap_count; ++a) {
        const std::size_t base = a * frame_count;
        for (std::size_t i = 0; i < frame_count; ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t l, std::size_t r) {
            const float ql = analysis.matrix[base + l].quality, qr = analysis.matrix[base + r].quality;
            if (ql != qr) return ql > qr;
            return analysis.analyzed_indices[l] < analysis.analyzed_indices[r];
        });
        int bins2[4] = {0, 0, 0, 0}, bins3[9] = {0};
        for (int k = 0; k < keep; ++k) {
            const LocalMatch& m = analysis.matrix[base + order[static_cast<std::size_t>(k)]];
            const double fx = m.dx - std::floor(m.dx), fy = m.dy - std::floor(m.dy);
            ++bins2[std::min(1, static_cast<int>(fy * 2)) * 2 + std::min(1, static_cast<int>(fx * 2))];
            ++bins3[std::min(2, static_cast<int>(fy * 3)) * 3 + std::min(2, static_cast<int>(fx * 3))];
        }
        // 升目に「均等に分けたときの1/4以上」が入っていれば埋まったとみなす。
        const double need2 = std::max(1.0, keep / 4.0 * 0.25), need3 = std::max(1.0, keep / 9.0 * 0.25);
        int filled2 = 0, filled3 = 0;
        for (int i = 0; i < 4; ++i) filled2 += bins2[i] >= need2 ? 1 : 0;
        for (int i = 0; i < 9; ++i) filled3 += bins3[i] >= need3 ? 1 : 0;
        cov2.push_back(filled2 / 4.0);
        cov3.push_back(filled3 / 9.0);
    }
    const auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    d.phase_coverage2 = median(cov2);
    d.phase_coverage3 = median(cov3);
    if (d.phase_coverage3 >= 0.75) d.limit_phase = 3.0;
    else if (d.phase_coverage2 >= 0.75) d.limit_phase = 2.0;
    else if (d.phase_coverage2 >= 0.5) d.limit_phase = 1.5;
    else d.limit_phase = 1.0;

    const double limits[3] = {d.limit_sampling, d.limit_phase, d.limit_frames};
    d.limiting = 0;
    for (int i = 1; i < 3; ++i) {
        if (limits[i] < limits[d.limiting]) d.limiting = i;
    }
    d.suggested_scale = snap_scale(limits[d.limiting]);
    return d;
}

}  // namespace stackcore
