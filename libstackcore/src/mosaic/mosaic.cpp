#include "stackcore/mosaic.hpp"

#include <Accelerate/Accelerate.h>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../common/parallel_rows.hpp"
#include "stackcore/resample.hpp"
#include "stackcore/zncc_matcher.hpp"

namespace stackcore {
namespace {

// ---- 定数 -------------------------------------------------------------------

// 粗い位置合わせに使う縮小画像の長辺（画素）。全変位の相関を取るので小さく保つ。
constexpr int kCoarseExtent = 256;
// 細かい位置合わせの1段目に使う縮小画像の長辺。大きなパネルでは、粗い候補の誤差
// （縮小率ぶん）と小さな回転を、等倍より広い範囲で一度に探す。
constexpr int kMiddleExtent = 1024;
// 粗い候補として認める重なりの最小幅（縮小画像の画素）と、面積の下限（小さい方のパネル比）。
constexpr int kMinCoarseOverlap = 6;
constexpr double kMinOverlapFraction = 0.03;
// 粗い候補として細かい確認へ進める相関の下限。当否は細かい位置合わせで決めるので緩くしてある。
constexpr double kMinCoarseNcc = 0.25;
// 対応点として認めるZNCCの下限。
constexpr double kMinPatchScore = 0.5;
// パネルどうしをつなぐのに必要な、互いに整合した対応点の数。
constexpr int kMinInliers = 4;
// 対応点を取る領域の最大数（1組あたり）。
constexpr int kMaxPatches = 196;
// 高い周波数の重みの鋭さ（縁からの距離の何乗か）。大きいほど継ぎ目の近くだけで混ざる。
constexpr int kHighBandPower = 6;

// ---- 小道具 -----------------------------------------------------------------

void notify(const ProgressFn& progress, const char* stage, int done, int total) {
    if (progress && !progress(stage, done, total)) throw Cancelled();
}

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

// 連続した1チャンネルの画像。
struct Plane {
    int w = 0;
    int h = 0;
    std::vector<float> v;
    const float* row(int y) const { return v.data() + static_cast<std::size_t>(y) * w; }
    float* row(int y) { return v.data() + static_cast<std::size_t>(y) * w; }
};

Plane luma_plane(const FrameBuffer& frame) {
    Plane p;
    p.w = frame.width();
    p.h = frame.height();
    p.v.resize(static_cast<std::size_t>(p.w) * p.h);
    const float* luma = frame.luma();
    for (int y = 0; y < p.h; ++y) {
        std::memcpy(p.row(y), luma + static_cast<std::size_t>(y) * frame.stride(),
                    sizeof(float) * static_cast<std::size_t>(p.w));
    }
    return p;
}

// factor×factor の平均で縮小する（端の余りは捨てる）。
Plane downsample(const Plane& src, int factor) {
    if (factor <= 1) return src;
    Plane out;
    out.w = std::max(1, src.w / factor);
    out.h = std::max(1, src.h / factor);
    out.v.assign(static_cast<std::size_t>(out.w) * out.h, 0.0f);
    const double inv = 1.0 / (static_cast<double>(factor) * factor);
    detail::parallel_rows(out.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* d = out.row(y);
            for (int x = 0; x < out.w; ++x) {
                double sum = 0.0;
                for (int yy = 0; yy < factor; ++yy) {
                    const int sy = std::min(src.h - 1, y * factor + yy);
                    const float* s = src.row(sy);
                    for (int xx = 0; xx < factor; ++xx) {
                        sum += s[std::min(src.w - 1, x * factor + xx)];
                    }
                }
                d[x] = static_cast<float>(sum * inv);
            }
        }
    });
    return out;
}

// 箱型ぼかしを3回（ガウスの近似）。端は端の値を複製する。
void box_blur3(float* data, int w, int h, std::size_t stride, int radius) {
    if (radius < 1 || w < 2 || h < 2) return;
    const double inv = 1.0 / (2 * radius + 1);
    for (int pass = 0; pass < 3; ++pass) {
        // 横
        detail::parallel_rows(h, [&](int y0, int y1) {
            std::vector<float> line(static_cast<std::size_t>(w));
            for (int y = y0; y < y1; ++y) {
                float* r = data + static_cast<std::size_t>(y) * stride;
                double acc = 0.0;
                for (int k = -radius; k <= radius; ++k) acc += r[std::min(w - 1, std::max(0, k))];
                for (int x = 0; x < w; ++x) {
                    line[static_cast<std::size_t>(x)] = static_cast<float>(acc * inv);
                    acc += r[std::min(w - 1, x + radius + 1)] - r[std::max(0, x - radius)];
                }
                std::memcpy(r, line.data(), sizeof(float) * static_cast<std::size_t>(w));
            }
        });
        // 縦（列ごとに独立。列を行の塊として分ける）
        detail::parallel_rows(w, [&](int x0, int x1) {
            std::vector<float> line(static_cast<std::size_t>(h));
            for (int x = x0; x < x1; ++x) {
                double acc = 0.0;
                for (int k = -radius; k <= radius; ++k) {
                    acc += data[static_cast<std::size_t>(std::min(h - 1, std::max(0, k))) * stride + x];
                }
                for (int y = 0; y < h; ++y) {
                    line[static_cast<std::size_t>(y)] = static_cast<float>(acc * inv);
                    acc += data[static_cast<std::size_t>(std::min(h - 1, y + radius + 1)) * stride + x] -
                           data[static_cast<std::size_t>(std::max(0, y - radius)) * stride + x];
                }
                for (int y = 0; y < h; ++y) {
                    data[static_cast<std::size_t>(y) * stride + x] = line[static_cast<std::size_t>(y)];
                }
            }
        });
    }
}

// 相似変換（回転・等方の倍率・平行移動）。X = a x - b y + tx, Y = b x + a y + ty。
struct Similarity {
    double a = 1.0, b = 0.0, tx = 0.0, ty = 0.0;
    void apply(double x, double y, double& X, double& Y) const {
        X = a * x - b * y + tx;
        Y = b * x + a * y + ty;
    }
    void invert(double X, double Y, double& x, double& y) const {
        const double det = a * a + b * b;
        const double dx = X - tx, dy = Y - ty;
        x = (a * dx + b * dy) / det;
        y = (-b * dx + a * dy) / det;
    }
};

// 対応点。p はパネルAの画素座標、q はパネルBの画素座標（同じ場所を指す）。
struct Match {
    double px = 0.0, py = 0.0, qx = 0.0, qy = 0.0;
    double score = 0.0;
};

double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    return v[mid];
}

// 対応点から p → q の相似変換を最小二乗で求める（rotation が false なら平行移動だけ）。
Similarity fit_similarity(const std::vector<Match>& m, bool rotation) {
    Similarity s;
    if (m.empty()) return s;
    double pcx = 0.0, pcy = 0.0, qcx = 0.0, qcy = 0.0, wsum = 0.0;
    for (const Match& k : m) {
        pcx += k.score * k.px;
        pcy += k.score * k.py;
        qcx += k.score * k.qx;
        qcy += k.score * k.qy;
        wsum += k.score;
    }
    pcx /= wsum;
    pcy /= wsum;
    qcx /= wsum;
    qcy /= wsum;
    double spp = 0.0, sdot = 0.0, scross = 0.0;
    for (const Match& k : m) {
        const double ax = k.px - pcx, ay = k.py - pcy, bx = k.qx - qcx, by = k.qy - qcy;
        spp += k.score * (ax * ax + ay * ay);
        sdot += k.score * (ax * bx + ay * by);
        scross += k.score * (ax * by - ay * bx);
    }
    // 点が1か所に固まっていると回転は決まらない。
    if (rotation && m.size() >= 3 && spp / wsum > 16.0 * 16.0) {
        s.a = sdot / spp;
        s.b = scross / spp;
    }
    s.tx = qcx - (s.a * pcx - s.b * pcy);
    s.ty = qcy - (s.b * pcx + s.a * pcy);
    return s;
}

// 外れた対応点を除きながら相似変換を求める。残った対応点を inliers に返す。
Similarity robust_fit(const std::vector<Match>& matches, bool rotation, double tolerance,
                      std::vector<Match>& inliers) {
    inliers.clear();
    if (matches.empty()) return Similarity();
    // まず平行移動の中央値に近いものだけを残す（偽ピークを最初に落とす）。
    std::vector<double> dx, dy;
    for (const Match& k : matches) {
        dx.push_back(k.qx - k.px);
        dy.push_back(k.qy - k.py);
    }
    const double mx = median_of(dx), my = median_of(dy);
    std::vector<double> dev;
    for (std::size_t i = 0; i < matches.size(); ++i) dev.push_back(std::hypot(dx[i] - mx, dy[i] - my));
    // 回転があると平行移動は場所によって違う。ばらつきに合わせて入口を広げる。
    const double gate = std::max(3.0 * tolerance, 3.0 * median_of(dev) + tolerance);
    for (std::size_t i = 0; i < matches.size(); ++i) {
        if (dev[i] <= gate) inliers.push_back(matches[i]);
    }
    Similarity s = fit_similarity(inliers, rotation);
    for (int iteration = 0; iteration < 4; ++iteration) {
        std::vector<double> residual;
        for (const Match& k : inliers) {
            double X, Y;
            s.apply(k.px, k.py, X, Y);
            residual.push_back(std::hypot(X - k.qx, Y - k.qy));
        }
        const double limit = std::max(tolerance, 2.5 * median_of(residual));
        std::vector<Match> kept;
        for (std::size_t i = 0; i < inliers.size(); ++i) {
            if (residual[i] <= limit) kept.push_back(inliers[i]);
        }
        const bool changed = kept.size() != inliers.size();
        inliers.swap(kept);
        if (inliers.empty()) return Similarity();
        s = fit_similarity(inliers, rotation);
        if (!changed) break;
    }
    // 最後に許容量の中に収まったものだけを数える。
    std::vector<Match> kept;
    for (const Match& k : inliers) {
        double X, Y;
        s.apply(k.px, k.py, X, Y);
        if (std::hypot(X - k.qx, Y - k.qy) <= tolerance) kept.push_back(k);
    }
    inliers.swap(kept);
    return s;
}

// ---- パネルの下ごしらえ -----------------------------------------------------

struct Panel {
    const FrameBuffer* frame = nullptr;
    int w = 0, h = 0;
    Plane middle;  // 細かい位置合わせの1段目（等倍で足りるなら空）

    // 粗い位置合わせ用
    int cw = 0, ch = 0;
    std::vector<float> coarse;           // 帯域通過した縮小画像
    std::vector<double> sum, sum_sq;     // 積分画像 (cw+1)×(ch+1)
    std::vector<float> fft_re, fft_im;   // P×P にゼロ詰めしたFFT
};

double rect_sum(const std::vector<double>& integral, int stride, int x0, int y0, int x1, int y1) {
    return integral[static_cast<std::size_t>(y1) * stride + x1] -
           integral[static_cast<std::size_t>(y0) * stride + x1] -
           integral[static_cast<std::size_t>(y1) * stride + x0] +
           integral[static_cast<std::size_t>(y0) * stride + x0];
}

void prepare_coarse(Panel& panel, const Plane& luma, int factor, int padded, FFTSetup setup) {
    Plane small = downsample(luma, factor);
    // 月面の大きな明暗（欠け際の陰り・周辺減光）を除く。残すのは地形の模様だけ。
    Plane low = small;
    box_blur3(low.v.data(), low.w, low.h, static_cast<std::size_t>(low.w), 4);
    for (std::size_t i = 0; i < small.v.size(); ++i) small.v[i] -= low.v[i];

    panel.cw = small.w;
    panel.ch = small.h;
    panel.coarse.swap(small.v);

    const int stride = panel.cw + 1;
    panel.sum.assign(static_cast<std::size_t>(stride) * (panel.ch + 1), 0.0);
    panel.sum_sq.assign(static_cast<std::size_t>(stride) * (panel.ch + 1), 0.0);
    for (int y = 0; y < panel.ch; ++y) {
        double row = 0.0, row_sq = 0.0;
        const float* s = panel.coarse.data() + static_cast<std::size_t>(y) * panel.cw;
        for (int x = 0; x < panel.cw; ++x) {
            row += s[x];
            row_sq += static_cast<double>(s[x]) * s[x];
            panel.sum[static_cast<std::size_t>(y + 1) * stride + x + 1] =
                panel.sum[static_cast<std::size_t>(y) * stride + x + 1] + row;
            panel.sum_sq[static_cast<std::size_t>(y + 1) * stride + x + 1] =
                panel.sum_sq[static_cast<std::size_t>(y) * stride + x + 1] + row_sq;
        }
    }

    const std::size_t total = static_cast<std::size_t>(padded) * padded;
    panel.fft_re.assign(total, 0.0f);
    panel.fft_im.assign(total, 0.0f);
    for (int y = 0; y < panel.ch; ++y) {
        std::memcpy(panel.fft_re.data() + static_cast<std::size_t>(y) * padded,
                    panel.coarse.data() + static_cast<std::size_t>(y) * panel.cw,
                    sizeof(float) * static_cast<std::size_t>(panel.cw));
    }
    DSPSplitComplex split{panel.fft_re.data(), panel.fft_im.data()};
    vDSP_fft2d_zip(setup, &split, 1, 0, log2_of(padded), log2_of(padded), FFT_FORWARD);
}

// ---- 粗い位置合わせ ---------------------------------------------------------

// パネルBをパネルAの座標で (sx, sy) に置く、という候補（縮小画像の画素）。
struct CoarseCandidate {
    int sx = 0, sy = 0;
    double ncc = -2.0;
    double rank = -2.0;
};

// 重なった部分だけで測った正規化相互相関を、すべての変位について求める。
// 分子はFFTの相互相関、重なりの和・2乗和は積分画像から引く（重なりは矩形なので）。
void coarse_candidates(const Panel& a, const Panel& b, int padded, FFTSetup setup,
                       std::vector<CoarseCandidate>& out) {
    out.clear();
    const std::size_t total = static_cast<std::size_t>(padded) * padded;
    std::vector<float> re(total), im(total);
    // corr(s) = Σ A(x) B(x - s) = IFFT( FA · conj(FB) )
    for (std::size_t i = 0; i < total; ++i) {
        re[i] = a.fft_re[i] * b.fft_re[i] + a.fft_im[i] * b.fft_im[i];
        im[i] = a.fft_im[i] * b.fft_re[i] - a.fft_re[i] * b.fft_im[i];
    }
    DSPSplitComplex split{re.data(), im.data()};
    vDSP_fft2d_zip(setup, &split, 1, 0, log2_of(padded), log2_of(padded), FFT_INVERSE);
    const double scale = 1.0 / (static_cast<double>(padded) * padded);

    const double min_area =
        kMinOverlapFraction * std::min(static_cast<double>(a.cw) * a.ch, static_cast<double>(b.cw) * b.ch);
    const int sa = a.cw + 1, sb = b.cw + 1;

    const int sx_lo = -(b.cw - kMinCoarseOverlap), sx_hi = a.cw - kMinCoarseOverlap;
    const int sy_lo = -(b.ch - kMinCoarseOverlap), sy_hi = a.ch - kMinCoarseOverlap;
    const int span_x = sx_hi - sx_lo + 1, span_y = sy_hi - sy_lo + 1;
    if (span_x <= 0 || span_y <= 0) return;
    std::vector<float> rank(static_cast<std::size_t>(span_x) * span_y, -2.0f);
    std::vector<float> value(static_cast<std::size_t>(span_x) * span_y, -2.0f);

    for (int sy = sy_lo; sy <= sy_hi; ++sy) {
        const int ay0 = std::max(0, sy), ay1 = std::min(a.ch, b.ch + sy);
        const int oh = ay1 - ay0;
        if (oh < kMinCoarseOverlap) continue;
        const int iy = sy < 0 ? sy + padded : sy;
        for (int sx = sx_lo; sx <= sx_hi; ++sx) {
            const int ax0 = std::max(0, sx), ax1 = std::min(a.cw, b.cw + sx);
            const int ow = ax1 - ax0;
            if (ow < kMinCoarseOverlap) continue;
            const double n = static_cast<double>(ow) * oh;
            if (n < min_area) continue;
            const int ix = sx < 0 ? sx + padded : sx;
            const double sab = static_cast<double>(re[static_cast<std::size_t>(iy) * padded + ix]) * scale;
            const double suma = rect_sum(a.sum, sa, ax0, ay0, ax1, ay1);
            const double sumaa = rect_sum(a.sum_sq, sa, ax0, ay0, ax1, ay1);
            const double sumb = rect_sum(b.sum, sb, ax0 - sx, ay0 - sy, ax1 - sx, ay1 - sy);
            const double sumbb = rect_sum(b.sum_sq, sb, ax0 - sx, ay0 - sy, ax1 - sx, ay1 - sy);
            const double va = sumaa - suma * suma / n;
            const double vb = sumbb - sumb * sumb / n;
            if (!(va > 1e-12) || !(vb > 1e-12)) continue;
            const double ncc = (sab - suma * sumb / n) / std::sqrt(va * vb);
            // 重なりが小さいほど、たまたま高い相関が出やすい。標本の少なさぶんを割り引いて比べる。
            const double penalized = ncc - 2.0 / std::sqrt(n / 16.0);
            const std::size_t i = static_cast<std::size_t>(sy - sy_lo) * span_x + (sx - sx_lo);
            value[i] = static_cast<float>(ncc);
            rank[i] = static_cast<float>(penalized);
        }
    }

    // 上位2つの山を取る（2つ目は1つ目から離れていること）。
    for (int pick = 0; pick < 2; ++pick) {
        CoarseCandidate best;
        for (int y = 0; y < span_y; ++y) {
            for (int x = 0; x < span_x; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * span_x + x;
                if (rank[i] <= best.rank) continue;
                bool near_taken = false;
                for (const CoarseCandidate& c : out) {
                    if (std::abs(c.sx - (x + sx_lo)) <= 8 && std::abs(c.sy - (y + sy_lo)) <= 8) near_taken = true;
                }
                if (near_taken) continue;
                best.rank = rank[i];
                best.ncc = value[i];
                best.sx = x + sx_lo;
                best.sy = y + sy_lo;
            }
        }
        if (best.ncc < kMinCoarseNcc) break;
        out.push_back(best);
    }
}

// ---- 細かい位置合わせ -------------------------------------------------------

// A の上に領域を敷き詰め、predict が示す B の位置のまわりで ZNCC を取る。
// 座標は画素の番号（その画像の縮尺）。対応点は領域の中心で、out の後ろに足す。
template <typename Predict>
void match_grid(const float* a, int aw, int ah, std::size_t astride, const float* b, int bw,
                int bh, std::size_t bstride, int tmpl, int radius, const Predict& predict,
                std::vector<Match>& out) {
    if (aw < tmpl || ah < tmpl || bw < tmpl || bh < tmpl) return;
    const int half = tmpl / 2;

    struct Site {
        int cx, cy, qx, qy;
    };
    std::vector<Site> sites;
    // 半分ずつ重ねて敷く。重なりが細い帯でも、対応点が1列ぶん多く取れる。
    int spacing = std::max(8, tmpl / 2);
    for (int attempt = 0; attempt < 2; ++attempt) {
        sites.clear();
        for (int cy = half; cy + half <= ah; cy += spacing) {
            for (int cx = half; cx + half <= aw; cx += spacing) {
                double qx = 0.0, qy = 0.0;
                predict(static_cast<double>(cx), static_cast<double>(cy), qx, qy);
                const int ix = static_cast<int>(std::lround(qx));
                const int iy = static_cast<int>(std::lround(qy));
                // 予測した位置で、領域が B の中に（少しの余裕つきで）収まる場所だけを使う。
                // 探索の窓は B の外へはみ出してよい（細い重なりを無駄にしない）。
                // はみ出した先で見つかった対応は、下で捨てる。
                if (ix - half - 2 < 0 || iy - half - 2 < 0 || ix + half + 2 > bw || iy + half + 2 > bh) {
                    continue;
                }
                sites.push_back(Site{cx, cy, ix, iy});
            }
        }
        if (static_cast<int>(sites.size()) <= kMaxPatches) break;
        spacing = static_cast<int>(
            std::ceil(spacing * std::sqrt(static_cast<double>(sites.size()) / kMaxPatches)));
    }
    if (sites.empty()) return;

    ZnccMatcher matcher(tmpl, radius);
    const int search_size = tmpl + 2 * radius;
    std::vector<float> patch(static_cast<std::size_t>(tmpl) * tmpl);
    std::vector<float> search(static_cast<std::size_t>(search_size) * search_size);
    for (const Site& site : sites) {
        copy_patch_clamped(a, aw, ah, astride, site.cx - half, site.cy - half, patch.data(), tmpl,
                           tmpl, static_cast<std::size_t>(tmpl));
        // 模様のない領域（空・影）は対応が決まらないので使わない。
        double sum = 0.0, sum_sq = 0.0;
        for (float v : patch) {
            sum += v;
            sum_sq += static_cast<double>(v) * v;
        }
        const double count = static_cast<double>(patch.size());
        const double variance = sum_sq / count - (sum / count) * (sum / count);
        if (!(variance > 1e-7)) continue;
        if (!matcher.set_template(patch.data(), static_cast<std::size_t>(tmpl))) continue;
        copy_patch_clamped(b, bw, bh, bstride, site.qx - half - radius, site.qy - half - radius,
                           search.data(), search_size, search_size,
                           static_cast<std::size_t>(search_size));
        const MatchResult r = matcher.match(search.data(), static_cast<std::size_t>(search_size));
        if (r.at_search_limit || r.score < kMinPatchScore) continue;
        // 見つかった位置で領域が B の外に出るなら、端の複製と比べただけなので使わない。
        if (site.qx - half + r.dx < -0.5 || site.qy - half + r.dy < -0.5 ||
            site.qx + half + r.dx > bw + 0.5 || site.qy + half + r.dy > bh + 0.5) {
            continue;
        }
        Match m;
        m.px = site.cx - 0.5;
        m.py = site.cy - 0.5;
        m.qx = site.qx + r.dx - 0.5;
        m.qy = site.qy + r.dy - 0.5;
        m.score = r.score;
        out.push_back(m);
    }
}

// 粗い候補を等倍の対応点で確かめる。合っていれば A→B の対応点（等倍の画素）を返す。
bool refine_pair(const Panel& a, const Panel& b, const Plane& a_luma, const Plane& b_luma,
                 const CoarseCandidate& candidate, int coarse_factor, int middle_factor,
                 bool rotation, std::vector<Match>& out) {
    out.clear();
    std::vector<Match> matches, inliers;

    // --- 1段目（縮小。大きなパネルのときだけ） ---
    // B を A の座標で s に置く → A の点 p は B の p - s。
    const double shift_x = static_cast<double>(candidate.sx) * coarse_factor;
    const double shift_y = static_cast<double>(candidate.sy) * coarse_factor;
    Similarity full;  // A → B（等倍）
    full.tx = -shift_x;
    full.ty = -shift_y;

    if (middle_factor > 1) {
        const double f = static_cast<double>(middle_factor);
        const int radius = std::min(40, static_cast<int>(std::ceil(1.5 * coarse_factor / f)) + 8);
        const auto predict = [&](double x, double y, double& qx, double& qy) {
            qx = x - shift_x / f;
            qy = y - shift_y / f;
        };
        matches.clear();
        for (const int size : {48, 24}) {
            match_grid(a.middle.v.data(), a.middle.w, a.middle.h, static_cast<std::size_t>(a.middle.w),
                       b.middle.v.data(), b.middle.w, b.middle.h, static_cast<std::size_t>(b.middle.w),
                       size, radius, predict, matches);
            if (static_cast<int>(matches.size()) >= 8) break;  // 重なりが細いときだけ小さな領域も使う
        }
        if (static_cast<int>(matches.size()) < 3) return false;
        const Similarity s1 = robust_fit(matches, rotation, 1.5, inliers);
        if (static_cast<int>(inliers.size()) < 3) return false;
        // 縮小画像の座標から等倍へ: p_full = f * p + c,  c = (f - 1) / 2。
        const double c = (f - 1.0) / 2.0;
        full.a = s1.a;
        full.b = s1.b;
        full.tx = f * s1.tx + c - s1.a * c + s1.b * c;
        full.ty = f * s1.ty + c - s1.b * c - s1.a * c;
    }

    // --- 2段目（等倍） ---
    const int radius = middle_factor > 1 ? std::min(24, 2 * middle_factor + 3)
                                         : std::min(40, static_cast<int>(std::ceil(1.5 * coarse_factor)) + 8);
    const auto predict = [&](double x, double y, double& qx, double& qy) {
        // match_grid は領域の左上を「番号 - 半分」で取る。予測は画素の番号どうしで行う。
        full.apply(x, y, qx, qy);
    };
    matches.clear();
    for (const int size : {64, 32}) {
        match_grid(a_luma.v.data(), a_luma.w, a_luma.h, static_cast<std::size_t>(a_luma.w),
                   b_luma.v.data(), b_luma.w, b_luma.h, static_cast<std::size_t>(b_luma.w), size, radius,
                   predict, matches);
        if (static_cast<int>(matches.size()) >= 8) break;
    }
    if (static_cast<int>(matches.size()) < 3) return false;
    robust_fit(matches, rotation, 2.0, inliers);
    const int needed = matches.size() >= 8 ? kMinInliers : 3;
    if (static_cast<int>(inliers.size()) < needed) return false;
    // 取れた対応点の大半が互いに食い違うなら、偶然の一致とみなす。
    if (inliers.size() * 3 < matches.size()) return false;
    out.swap(inliers);
    return true;
}

// ---- 全体の調整 -------------------------------------------------------------

struct Link {
    int i = 0, j = 0;
    std::vector<Match> matches;  // p は i の画素、q は j の画素
};

// 正規方程式を足し込みながら解く小さな最小二乗。
class LeastSquares {
public:
    explicit LeastSquares(int unknowns)
        : n_(unknowns), normal_(static_cast<std::size_t>(unknowns) * unknowns, 0.0),
          rhs_(static_cast<std::size_t>(unknowns), 0.0) {}

    // Σ coef[k] * x[index[k]] = value を重み w で足す。index が負の項は無視する。
    void add(const int* index, const double* coef, int count, double value, double w) {
        for (int r = 0; r < count; ++r) {
            if (index[r] < 0) continue;
            rhs_[static_cast<std::size_t>(index[r])] += w * coef[r] * value;
            for (int c = 0; c < count; ++c) {
                if (index[c] < 0) continue;
                normal_[static_cast<std::size_t>(index[r]) * n_ + index[c]] += w * coef[r] * coef[c];
            }
        }
    }

    // ガウスの消去法（部分ピボット）。解けなければ false。
    bool solve(std::vector<double>& x) {
        std::vector<double> m = normal_;
        x = rhs_;
        for (int i = 0; i < n_; ++i) m[static_cast<std::size_t>(i) * n_ + i] += 1e-9;
        for (int col = 0; col < n_; ++col) {
            int pivot = col;
            double best = std::fabs(m[static_cast<std::size_t>(col) * n_ + col]);
            for (int r = col + 1; r < n_; ++r) {
                const double v = std::fabs(m[static_cast<std::size_t>(r) * n_ + col]);
                if (v > best) {
                    best = v;
                    pivot = r;
                }
            }
            if (!(best > 1e-14)) return false;
            if (pivot != col) {
                for (int c = 0; c < n_; ++c) {
                    std::swap(m[static_cast<std::size_t>(pivot) * n_ + c],
                              m[static_cast<std::size_t>(col) * n_ + c]);
                }
                std::swap(x[static_cast<std::size_t>(pivot)], x[static_cast<std::size_t>(col)]);
            }
            const double diag = m[static_cast<std::size_t>(col) * n_ + col];
            for (int r = col + 1; r < n_; ++r) {
                const double factor = m[static_cast<std::size_t>(r) * n_ + col] / diag;
                if (factor == 0.0) continue;
                for (int c = col; c < n_; ++c) {
                    m[static_cast<std::size_t>(r) * n_ + c] -= factor * m[static_cast<std::size_t>(col) * n_ + c];
                }
                x[static_cast<std::size_t>(r)] -= factor * x[static_cast<std::size_t>(col)];
            }
        }
        for (int row = n_ - 1; row >= 0; --row) {
            double v = x[static_cast<std::size_t>(row)];
            for (int c = row + 1; c < n_; ++c) {
                v -= m[static_cast<std::size_t>(row) * n_ + c] * x[static_cast<std::size_t>(c)];
            }
            x[static_cast<std::size_t>(row)] = v / m[static_cast<std::size_t>(row) * n_ + row];
        }
        return true;
    }

private:
    int n_;
    std::vector<double> normal_;
    std::vector<double> rhs_;
};

// つながったパネルの集まりのうち、いちばん大きいものを選ぶ。
// member[i] が true のパネルを配置する。anchor は基準にするパネル。
void largest_component(int count, const std::vector<Link>& links, std::vector<bool>& member,
                       int& anchor) {
    std::vector<int> parent(static_cast<std::size_t>(count));
    std::iota(parent.begin(), parent.end(), 0);
    const auto find = [&parent](int v) {
        while (parent[static_cast<std::size_t>(v)] != v) {
            parent[static_cast<std::size_t>(v)] =
                parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(v)])];
            v = parent[static_cast<std::size_t>(v)];
        }
        return v;
    };
    for (const Link& link : links) {
        const int a = find(link.i), b = find(link.j);
        if (a != b) parent[static_cast<std::size_t>(std::max(a, b))] = std::min(a, b);
    }
    std::vector<int> size(static_cast<std::size_t>(count), 0);
    for (int i = 0; i < count; ++i) ++size[static_cast<std::size_t>(find(i))];
    int best = 0;
    for (int i = 1; i < count; ++i) {
        if (size[static_cast<std::size_t>(i)] > size[static_cast<std::size_t>(best)]) best = i;
    }
    member.assign(static_cast<std::size_t>(count), false);
    for (int i = 0; i < count; ++i) member[static_cast<std::size_t>(i)] = find(i) == best;
    anchor = best;  // 根はその集まりの最小の番号
}

// すべての対応点を同時に満たす配置を解く。transforms[i] はパネル i → 基準パネルの座標。
// rotation が false なら平行移動だけ。
bool solve_layout(const std::vector<Panel>& panels, const std::vector<Link>& links,
                  const std::vector<bool>& member, int anchor, bool rotation,
                  std::vector<Similarity>& transforms) {
    const int count = static_cast<int>(panels.size());
    const int per = rotation ? 4 : 2;
    std::vector<int> slot(static_cast<std::size_t>(count), -1);
    int unknowns = 0;
    for (int i = 0; i < count; ++i) {
        if (!member[static_cast<std::size_t>(i)] || i == anchor) continue;
        slot[static_cast<std::size_t>(i)] = unknowns;
        unknowns += per;
    }
    transforms.assign(static_cast<std::size_t>(count), Similarity());
    if (unknowns == 0) return true;

    // 座標はパネルの中心から測る（回転の係数と平行移動の係数の大きさを近づけるため）。
    // T_k(x) = A_k (x - c_k) + t_k。t_k は「パネルの中心がどこに来るか」。
    LeastSquares ls(unknowns);
    for (const Link& link : links) {
        if (!member[static_cast<std::size_t>(link.i)] || !member[static_cast<std::size_t>(link.j)]) continue;
        const Panel& pi = panels[static_cast<std::size_t>(link.i)];
        const Panel& pj = panels[static_cast<std::size_t>(link.j)];
        const double cix = (pi.w - 1) * 0.5, ciy = (pi.h - 1) * 0.5;
        const double cjx = (pj.w - 1) * 0.5, cjy = (pj.h - 1) * 0.5;
        const int si = slot[static_cast<std::size_t>(link.i)];
        const int sj = slot[static_cast<std::size_t>(link.j)];
        for (const Match& m : link.matches) {
            const double px = m.px - cix, py = m.py - ciy;
            const double qx = m.qx - cjx, qy = m.qy - cjy;
            const double w = m.score;
            if (rotation) {
                // X: (a_i px - b_i py + tx_i) - (a_j qx - b_j qy + tx_j) = 0
                // Y: (b_i px + a_i py + ty_i) - (b_j qx + a_j qy + ty_j) = 0
                // 基準パネルは a = 1, b = 0, t = 中心。定数として右辺へ移す。
                int index[8];
                double coef[8];
                double value = 0.0;
                index[0] = si < 0 ? -1 : si + 0; coef[0] = px;
                index[1] = si < 0 ? -1 : si + 1; coef[1] = -py;
                index[2] = si < 0 ? -1 : si + 2; coef[2] = 1.0;
                index[3] = sj < 0 ? -1 : sj + 0; coef[3] = -qx;
                index[4] = sj < 0 ? -1 : sj + 1; coef[4] = qy;
                index[5] = sj < 0 ? -1 : sj + 2; coef[5] = -1.0;
                if (si < 0) value -= px + cix;
                if (sj < 0) value += qx + cjx;
                ls.add(index, coef, 6, value, w);

                value = 0.0;
                index[0] = si < 0 ? -1 : si + 1; coef[0] = px;
                index[1] = si < 0 ? -1 : si + 0; coef[1] = py;
                index[2] = si < 0 ? -1 : si + 3; coef[2] = 1.0;
                index[3] = sj < 0 ? -1 : sj + 1; coef[3] = -qx;
                index[4] = sj < 0 ? -1 : sj + 0; coef[4] = -qy;
                index[5] = sj < 0 ? -1 : sj + 3; coef[5] = -1.0;
                if (si < 0) value -= py + ciy;
                if (sj < 0) value += qy + cjy;
                ls.add(index, coef, 6, value, w);
            } else {
                // tx_i + px - tx_j - qx = 0（a = 1, b = 0）
                int index[2];
                double coef[2];
                index[0] = si < 0 ? -1 : si + 0; coef[0] = 1.0;
                index[1] = sj < 0 ? -1 : sj + 0; coef[1] = -1.0;
                double value = qx - px;
                if (si < 0) value -= cix;
                if (sj < 0) value += cjx;
                ls.add(index, coef, 2, value, w);
                index[0] = si < 0 ? -1 : si + 1;
                index[1] = sj < 0 ? -1 : sj + 1;
                value = qy - py;
                if (si < 0) value -= ciy;
                if (sj < 0) value += cjy;
                ls.add(index, coef, 2, value, w);
            }
        }
    }
    std::vector<double> x;
    if (!ls.solve(x)) return false;

    for (int i = 0; i < count; ++i) {
        const int s = slot[static_cast<std::size_t>(i)];
        if (s < 0) continue;
        const Panel& p = panels[static_cast<std::size_t>(i)];
        const double cx = (p.w - 1) * 0.5, cy = (p.h - 1) * 0.5;
        Similarity t;
        double tcx, tcy;
        if (rotation) {
            t.a = x[static_cast<std::size_t>(s)];
            t.b = x[static_cast<std::size_t>(s) + 1];
            tcx = x[static_cast<std::size_t>(s) + 2];
            tcy = x[static_cast<std::size_t>(s) + 3];
        } else {
            tcx = x[static_cast<std::size_t>(s)];
            tcy = x[static_cast<std::size_t>(s) + 1];
        }
        // T(x) = A (x - c) + tc  →  平行移動は tc - A c。
        t.tx = tcx - (t.a * cx - t.b * cy);
        t.ty = tcy - (t.b * cx + t.a * cy);
        transforms[static_cast<std::size_t>(i)] = t;
    }
    return true;
}

double link_rms(const Link& link, const std::vector<Similarity>& transforms) {
    double sum = 0.0;
    for (const Match& m : link.matches) {
        double ax, ay, bx, by;
        transforms[static_cast<std::size_t>(link.i)].apply(m.px, m.py, ax, ay);
        transforms[static_cast<std::size_t>(link.j)].apply(m.qx, m.qy, bx, by);
        sum += (ax - bx) * (ax - bx) + (ay - by) * (ay - by);
    }
    return link.matches.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(link.matches.size()));
}

// ---- 標本化 -----------------------------------------------------------------

// Lanczos3 の重み表（小数位置を1/1024画素刻みで持つ。各行の和は1）。
struct LanczosTable {
    static constexpr int kPhases = 1024;
    std::vector<float> weights;  // kPhases × 6
    LanczosTable() : weights(static_cast<std::size_t>(kPhases) * 6) {
        for (int p = 0; p < kPhases; ++p) {
            const double frac = static_cast<double>(p) / kPhases;
            double w[6], sum = 0.0;
            for (int t = 0; t < 6; ++t) {
                w[t] = lanczos3_kernel(static_cast<double>(t - 2) - frac);
                sum += w[t];
            }
            for (int t = 0; t < 6; ++t) {
                weights[static_cast<std::size_t>(p) * 6 + t] = static_cast<float>(w[t] / sum);
            }
        }
    }
};

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// (u, v) を Lanczos3 で標本化する。端は端の値を複製する。
inline float sample_lanczos(const float* plane, int w, int h, std::size_t stride, double u,
                            double v, const LanczosTable& table) {
    double fu = std::floor(u), fv = std::floor(v);
    int iu = static_cast<int>(fu), iv = static_cast<int>(fv);
    int pu = static_cast<int>(std::lround((u - fu) * LanczosTable::kPhases));
    int pv = static_cast<int>(std::lround((v - fv) * LanczosTable::kPhases));
    if (pu == LanczosTable::kPhases) {
        pu = 0;
        ++iu;
    }
    if (pv == LanczosTable::kPhases) {
        pv = 0;
        ++iv;
    }
    if (pu == 0 && pv == 0) {
        return plane[static_cast<std::size_t>(clampi(iv, 0, h - 1)) * stride + clampi(iu, 0, w - 1)];
    }
    const float* wx = table.weights.data() + static_cast<std::size_t>(pu) * 6;
    const float* wy = table.weights.data() + static_cast<std::size_t>(pv) * 6;
    int xs[6];
    for (int t = 0; t < 6; ++t) xs[t] = clampi(iu + t - 2, 0, w - 1);
    float acc = 0.0f;
    for (int ty = 0; ty < 6; ++ty) {
        const float* row = plane + static_cast<std::size_t>(clampi(iv + ty - 2, 0, h - 1)) * stride;
        float line = 0.0f;
        for (int t = 0; t < 6; ++t) line += wx[t] * row[xs[t]];
        acc += wy[ty] * line;
    }
    return acc;
}

inline float sample_bilinear(const float* plane, int w, int h, std::size_t stride, double u,
                             double v) {
    const double fu = std::floor(u), fv = std::floor(v);
    const int x0 = clampi(static_cast<int>(fu), 0, w - 1), x1 = clampi(static_cast<int>(fu) + 1, 0, w - 1);
    const int y0 = clampi(static_cast<int>(fv), 0, h - 1), y1 = clampi(static_cast<int>(fv) + 1, 0, h - 1);
    const float ax = static_cast<float>(u - fu), ay = static_cast<float>(v - fv);
    const float* r0 = plane + static_cast<std::size_t>(y0) * stride;
    const float* r1 = plane + static_cast<std::size_t>(y1) * stride;
    const float top = r0[x0] + (r0[x1] - r0[x0]) * ax;
    const float bottom = r1[x0] + (r1[x1] - r1[x0]) * ax;
    return top + (bottom - top) * ay;
}

// パネルの暗い背景（空）の明るさ。カメラの下駄（オフセット）に空の明るさが乗ったもの。
//
// 明るさ合わせの倍率は「背景より上」にだけ掛けなければならない。透明度や露出の差で
// 変わるのは月の光で、カメラの下駄は変わらないからである。下駄ごと倍率を掛けると、
// 月は合っても空の明るさがパネルごとに変わり、黒レベルを詰めて見たときに
// 空の段差と、重みの折れ目（パネルの角から伸びる斜めの筋）が見えてしまう
// （実写の2パネルで、下駄 0.016・倍率 0.95 と 1.05 のとき空が 0.0155 と 0.0172）。
//
// 背景があると認めるのは、暗い側の1割がほぼ同じ明るさ（平らな空）で、かつ
// パネルの明るい部分よりずっと暗いときだけ。月面の内側だけを写したパネルでは
// 暗部は影や海であって背景ではないので、認めない。
struct Background {
    bool found = false;
    double luma = 0.0;
    double channel[3] = {0.0, 0.0, 0.0};
};

Background estimate_background(const FrameBuffer& frame, const Plane& luma) {
    Background out;
    const int step = std::max(1, std::min(luma.w, luma.h) / 400);
    std::vector<float> values;
    for (int y = 0; y < luma.h; y += step) {
        const float* row = luma.row(y);
        for (int x = 0; x < luma.w; x += step) {
            if (row[x] > 0.0f) values.push_back(row[x]);  // 値のない縁（真っ黒）は数えない
        }
    }
    if (values.size() < 1024) return out;
    std::sort(values.begin(), values.end());
    const double p1 = values[values.size() / 100];
    const double p10 = values[values.size() / 10];
    const double p99 = values[values.size() - 1 - values.size() / 100];
    if (p10 - p1 > 0.15 * p10 + 0.0005) return out;  // 暗部が平らでない
    if (p10 > 0.25 * p99) return out;                // 明るい部分との差が小さい

    // 暗い側1割の画素の中央値を、輝度とチャンネルごとに取る。
    std::vector<float> dark_luma;
    std::vector<float> dark[3];
    const int channels = frame.channels() >= 3 ? 3 : 1;
    for (int y = 0; y < luma.h; y += step) {
        const float* row = luma.row(y);
        for (int x = 0; x < luma.w; x += step) {
            if (!(row[x] > 0.0f) || row[x] > p10) continue;
            dark_luma.push_back(row[x]);
            for (int c = 0; c < channels; ++c) dark[c].push_back(frame.row(c, y)[x]);
        }
    }
    if (dark_luma.empty()) return out;
    const auto middle = [](std::vector<float>& v) {
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return static_cast<double>(v[v.size() / 2]);
    };
    out.luma = middle(dark_luma);
    for (int c = 0; c < 3; ++c) out.channel[c] = channels == 3 ? middle(dark[c]) : 0.0;
    if (channels == 1) out.channel[0] = out.channel[1] = out.channel[2] = middle(dark[0]);
    out.found = true;
    return out;
}

// 対応の取れた重なりから、パネル i と j の平均輝度を測る。
bool overlap_means(const Panel& pi, const Plane& li, const Similarity& ti, const Panel& pj,
                   const Plane& lj, const Similarity& tj, double& mean_i, double& mean_j) {
    const int step = std::max(1, std::min(pi.w, pi.h) / 240);
    double si = 0.0, sj = 0.0;
    long long count = 0;
    for (int y = 1; y < pi.h - 1; y += step) {
        const float* row = li.row(y);
        for (int x = 1; x < pi.w - 1; x += step) {
            double X, Y, u, v;
            ti.apply(x, y, X, Y);
            tj.invert(X, Y, u, v);
            const int iu = static_cast<int>(std::lround(u)), iv = static_cast<int>(std::lround(v));
            if (iu < 1 || iv < 1 || iu >= pj.w - 1 || iv >= pj.h - 1) continue;
            const float a = row[x], b = lj.row(iv)[iu];
            if (!(a > 0.0f) || !(b > 0.0f)) continue;  // 値のない縁（真っ黒）は数えない
            si += a;
            sj += b;
            ++count;
        }
    }
    if (count < 64) return false;
    mean_i = si / static_cast<double>(count);
    mean_j = sj / static_cast<double>(count);
    return mean_i > 1e-5 && mean_j > 1e-5;
}

// 進捗の通知と中断を直列化しながら、項目を並列に処理する。
template <typename Fn>
void parallel_items(int count, const char* stage, const ProgressFn& progress, const Fn& fn) {
    struct Context {
        const Fn* function = nullptr;
        const ProgressFn* progress = nullptr;
        const char* stage = nullptr;
        int total = 0;
        int done = 0;
        std::atomic<bool> stop{false};
        bool cancelled = false;
        std::mutex mutex;
        std::exception_ptr failure;
    } context;
    context.function = &fn;
    context.progress = &progress;
    context.stage = stage;
    context.total = count;
    if (count <= 0) return;

    dispatch_apply_f(static_cast<std::size_t>(count),
                     dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), &context,
                     [](void* raw, std::size_t i) {
                         Context* c = static_cast<Context*>(raw);
                         if (c->stop.load(std::memory_order_relaxed)) return;
                         try {
                             (*c->function)(static_cast<int>(i));
                         } catch (...) {
                             std::lock_guard<std::mutex> lock(c->mutex);
                             if (!c->failure) c->failure = std::current_exception();
                             c->stop.store(true, std::memory_order_relaxed);
                             return;
                         }
                         std::lock_guard<std::mutex> lock(c->mutex);
                         ++c->done;
                         if (*c->progress && !(*c->progress)(c->stage, c->done, c->total)) {
                             c->cancelled = true;
                             c->stop.store(true, std::memory_order_relaxed);
                         }
                     });
    if (context.failure) std::rethrow_exception(context.failure);
    if (context.cancelled) throw Cancelled();
}

}  // namespace

FrameBuffer build_mosaic(const std::vector<const FrameBuffer*>& inputs,
                         const MosaicSettings& settings, const ProgressFn& progress,
                         MosaicReport& report) {
    report = MosaicReport();
    const int count = static_cast<int>(inputs.size());
    if (count < 2) throw std::runtime_error("モザイク合成: パネルが2枚以上必要です");
    if (count > 200) throw std::runtime_error("モザイク合成: パネルが多すぎます（200枚まで）");

    int max_extent = 0;
    bool any_color = false;
    for (const FrameBuffer* f : inputs) {
        if (f == nullptr || f->empty()) throw std::runtime_error("モザイク合成: 空のパネルがあります");
        if (f->width() < 96 || f->height() < 96) {
            throw std::runtime_error("モザイク合成: パネルが小さすぎます（縦横96画素以上が必要です）");
        }
        if (f->channels() != 1 && f->channels() != 3) {
            throw std::runtime_error("モザイク合成: 1チャンネルまたは3チャンネルの画像が必要です");
        }
        max_extent = std::max(max_extent, std::max(f->width(), f->height()));
        any_color = any_color || f->channels() == 3;
    }
    const int channels = any_color ? 3 : 1;
    const int coarse_factor = std::max(1, (max_extent + kCoarseExtent - 1) / kCoarseExtent);
    const int middle_factor = std::max(1, (max_extent + kMiddleExtent - 1) / kMiddleExtent);

    // ---- 下ごしらえ ---------------------------------------------------------
    std::vector<Panel> panels(static_cast<std::size_t>(count));
    std::vector<Plane> lumas(static_cast<std::size_t>(count));
    int coarse_extent = 0;
    for (int i = 0; i < count; ++i) {
        Panel& p = panels[static_cast<std::size_t>(i)];
        p.frame = inputs[static_cast<std::size_t>(i)];
        p.w = p.frame->width();
        p.h = p.frame->height();
        lumas[static_cast<std::size_t>(i)] = luma_plane(*p.frame);
        coarse_extent = std::max(coarse_extent, std::max(p.w, p.h) / coarse_factor + 1);
    }
    const int padded = next_pow2(2 * coarse_extent);
    FFTSetup setup = vDSP_create_fftsetup(log2_of(padded), FFT_RADIX2);
    if (setup == nullptr) throw std::runtime_error("モザイク合成: FFTの準備に失敗しました");
    struct SetupGuard {
        FFTSetup s;
        ~SetupGuard() { vDSP_destroy_fftsetup(s); }
    } guard{setup};

    for (int i = 0; i < count; ++i) {
        Panel& p = panels[static_cast<std::size_t>(i)];
        prepare_coarse(p, lumas[static_cast<std::size_t>(i)], coarse_factor, padded, setup);
        if (middle_factor > 1) p.middle = downsample(lumas[static_cast<std::size_t>(i)], middle_factor);
        notify(progress, "モザイク: パネルの準備", i + 1, count);
    }

    // ---- 位置合わせ（全組み合わせ） -----------------------------------------
    std::vector<std::pair<int, int>> pairs;
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) pairs.emplace_back(i, j);
    }
    std::vector<Link> found(pairs.size());
    std::vector<char> linked(pairs.size(), 0);
    parallel_items(static_cast<int>(pairs.size()), "モザイク: 位置合わせ", progress, [&](int k) {
        const int i = pairs[static_cast<std::size_t>(k)].first;
        const int j = pairs[static_cast<std::size_t>(k)].second;
        std::vector<CoarseCandidate> candidates;
        coarse_candidates(panels[static_cast<std::size_t>(i)], panels[static_cast<std::size_t>(j)], padded,
                          setup, candidates);
        for (const CoarseCandidate& candidate : candidates) {
            std::vector<Match> matches;
            if (refine_pair(panels[static_cast<std::size_t>(i)], panels[static_cast<std::size_t>(j)],
                            lumas[static_cast<std::size_t>(i)], lumas[static_cast<std::size_t>(j)], candidate,
                            coarse_factor, middle_factor, settings.allow_rotation, matches)) {
                Link& link = found[static_cast<std::size_t>(k)];
                link.i = i;
                link.j = j;
                link.matches.swap(matches);
                linked[static_cast<std::size_t>(k)] = 1;
                break;
            }
        }
    });
    std::vector<Link> links;
    for (std::size_t k = 0; k < found.size(); ++k) {
        if (linked[k]) links.push_back(std::move(found[k]));
    }
    // 中間の縮小画像はもう使わない。
    for (Panel& p : panels) {
        Plane().v.swap(p.middle.v);
        std::vector<float>().swap(p.coarse);
        std::vector<float>().swap(p.fft_re);
        std::vector<float>().swap(p.fft_im);
    }
    if (links.empty()) {
        throw std::runtime_error(
            "モザイク合成: パネルどうしの重なりを見つけられませんでした。"
            "隣り合うパネルが十分に（1〜2割以上）重なっているか確かめてください");
    }

    // ---- 全体の調整 ---------------------------------------------------------
    std::vector<bool> member;
    int anchor = 0;
    std::vector<Similarity> transforms;
    bool use_rotation = settings.allow_rotation;
    for (int round = 0; round < 3; ++round) {
        largest_component(count, links, member, anchor);
        if (!solve_layout(panels, links, member, anchor, use_rotation, transforms)) {
            if (use_rotation) {
                use_rotation = false;
                continue;
            }
            throw std::runtime_error("モザイク合成: パネルの配置を解けませんでした");
        }
        // ほかと食い違う組（偶然の一致）が紛れていたら外して解き直す。
        std::vector<double> rms;
        for (const Link& link : links) {
            if (member[static_cast<std::size_t>(link.i)] && member[static_cast<std::size_t>(link.j)]) {
                rms.push_back(link_rms(link, transforms));
            }
        }
        const double limit = std::max(4.0, 4.0 * median_of(rms));
        std::vector<Link> kept;
        bool dropped = false;
        for (Link& link : links) {
            const bool in = member[static_cast<std::size_t>(link.i)] && member[static_cast<std::size_t>(link.j)];
            if (in && link_rms(link, transforms) > limit) {
                dropped = true;
                continue;
            }
            kept.push_back(std::move(link));
        }
        links.swap(kept);
        if (!dropped) break;
        if (links.empty()) throw std::runtime_error("モザイク合成: パネルの配置を解けませんでした");
    }

    // 回転・倍率の補正が画素の半分にも満たないなら、平行移動だけで置き直す
    // （赤道儀で撮ったモザイクに、意味のないわずかな回転を持ち込まない）。
    if (use_rotation) {
        double worst = 0.0;
        for (int i = 0; i < count; ++i) {
            if (!member[static_cast<std::size_t>(i)]) continue;
            const Similarity& t = transforms[static_cast<std::size_t>(i)];
            const Panel& p = panels[static_cast<std::size_t>(i)];
            worst = std::max(worst, std::max(std::fabs(t.b), std::fabs(t.a - 1.0)) * 0.5 * std::hypot(p.w, p.h));
        }
        if (worst < 0.35) {
            use_rotation = false;
            if (!solve_layout(panels, links, member, anchor, false, transforms)) {
                throw std::runtime_error("モザイク合成: パネルの配置を解けませんでした");
            }
        }
    }

    // ---- 明るさ合わせ -------------------------------------------------------
    std::vector<double> gains(static_cast<std::size_t>(count), 1.0);
    std::vector<int> link_count(static_cast<std::size_t>(count), 0);
    for (const Link& link : links) {
        if (!member[static_cast<std::size_t>(link.i)] || !member[static_cast<std::size_t>(link.j)]) continue;
        ++link_count[static_cast<std::size_t>(link.i)];
        ++link_count[static_cast<std::size_t>(link.j)];
    }
    // 背景（下駄）。パネルごとの値と、合成後にそろえる共通の値。
    // 背景を認められたパネルが無ければ全部0で、倍率を画素値そのものに掛ける。
    std::vector<Background> backgrounds(static_cast<std::size_t>(count));
    Background common;
    if (settings.match_brightness) {
        int found = 0;
        for (int i = 0; i < count; ++i) {
            if (!member[static_cast<std::size_t>(i)]) continue;
            Background& b = backgrounds[static_cast<std::size_t>(i)];
            b = estimate_background(*panels[static_cast<std::size_t>(i)].frame,
                                    lumas[static_cast<std::size_t>(i)]);
            if (!b.found) continue;
            ++found;
            common.luma += b.luma;
            for (int c = 0; c < 3; ++c) common.channel[c] += b.channel[c];
        }
        if (found > 0) {
            common.found = true;
            common.luma /= found;
            for (int c = 0; c < 3; ++c) common.channel[c] /= found;
            // 背景が写っていないパネル（月面の内側）には、同じカメラの下駄として共通の値を使う。
            for (int i = 0; i < count; ++i) {
                Background& b = backgrounds[static_cast<std::size_t>(i)];
                if (member[static_cast<std::size_t>(i)] && !b.found) b = common;
            }
        }
    }

    if (settings.match_brightness) {
        LeastSquares ls(count);
        bool any = false;
        for (const Link& link : links) {
            if (!member[static_cast<std::size_t>(link.i)] || !member[static_cast<std::size_t>(link.j)]) continue;
            double mi = 0.0, mj = 0.0;
            if (!overlap_means(panels[static_cast<std::size_t>(link.i)], lumas[static_cast<std::size_t>(link.i)],
                               transforms[static_cast<std::size_t>(link.i)],
                               panels[static_cast<std::size_t>(link.j)], lumas[static_cast<std::size_t>(link.j)],
                               transforms[static_cast<std::size_t>(link.j)], mi, mj)) {
                continue;
            }
            // 背景より上の明るさどうしを合わせる。
            mi -= backgrounds[static_cast<std::size_t>(link.i)].luma;
            mj -= backgrounds[static_cast<std::size_t>(link.j)].luma;
            if (!(mi > 1e-5) || !(mj > 1e-5)) continue;  // 重なりが空だけ
            // g_i * m_i = g_j * m_j  →  log g_i - log g_j = log(m_j / m_i)
            const int index[2] = {link.i, link.j};
            const double coef[2] = {1.0, -1.0};
            ls.add(index, coef, 2, std::log(mj / mi), 1.0);
            any = true;
        }
        if (any) {
            // 全体の明るさは決まらないので、倍率を1から離さない弱い条件を足す。
            for (int i = 0; i < count; ++i) {
                const int index[1] = {i};
                const double coef[1] = {1.0};
                ls.add(index, coef, 1, 0.0, 1e-3);
            }
            std::vector<double> x;
            if (ls.solve(x)) {
                double mean = 0.0;
                int n = 0;
                for (int i = 0; i < count; ++i) {
                    if (!member[static_cast<std::size_t>(i)]) continue;
                    mean += x[static_cast<std::size_t>(i)];
                    ++n;
                }
                mean /= std::max(1, n);
                // 白飛びを作らない。明るい画素が1を超えるなら、全体を同じ割合で下げる。
                double peak = 0.0;
                for (int i = 0; i < count; ++i) {
                    if (!member[static_cast<std::size_t>(i)]) continue;
                    const double g = std::exp(std::min(1.5, std::max(-1.5, x[static_cast<std::size_t>(i)] - mean)));
                    gains[static_cast<std::size_t>(i)] = g;
                    float top = 0.0f;
                    for (float v : lumas[static_cast<std::size_t>(i)].v) top = std::max(top, v);
                    peak = std::max(peak, g * (top - backgrounds[static_cast<std::size_t>(i)].luma));
                }
                const double room = 1.0 - common.luma;
                if (peak > room && peak > 0.0) {
                    for (int i = 0; i < count; ++i) gains[static_cast<std::size_t>(i)] *= room / peak;
                }
            }
        }
    }

    // ---- 出力の大きさ -------------------------------------------------------
    double min_x = 1e30, min_y = 1e30, max_x = -1e30, max_y = -1e30;
    for (int i = 0; i < count; ++i) {
        if (!member[static_cast<std::size_t>(i)]) continue;
        const Panel& p = panels[static_cast<std::size_t>(i)];
        const double corners[4][2] = {{0.0, 0.0},
                                      {static_cast<double>(p.w - 1), 0.0},
                                      {0.0, static_cast<double>(p.h - 1)},
                                      {static_cast<double>(p.w - 1), static_cast<double>(p.h - 1)}};
        for (const auto& c : corners) {
            double X, Y;
            transforms[static_cast<std::size_t>(i)].apply(c[0], c[1], X, Y);
            min_x = std::min(min_x, X);
            min_y = std::min(min_y, Y);
            max_x = std::max(max_x, X);
            max_y = std::max(max_y, Y);
        }
    }
    const double origin_x = std::floor(min_x + 1e-6), origin_y = std::floor(min_y + 1e-6);
    const double span_w = std::ceil(max_x - 1e-6) - origin_x + 1.0;
    const double span_h = std::ceil(max_y - 1e-6) - origin_y + 1.0;
    if (span_w > 60000.0 || span_h > 60000.0 || span_w * span_h > 600e6) {
        throw std::runtime_error("モザイク合成: 出力が大きすぎます（6億画素まで）");
    }
    const int out_w = static_cast<int>(span_w), out_h = static_cast<int>(span_h);
    for (int i = 0; i < count; ++i) {
        transforms[static_cast<std::size_t>(i)].tx -= origin_x;
        transforms[static_cast<std::size_t>(i)].ty -= origin_y;
    }

    // ---- 合成 ---------------------------------------------------------------
    //
    // 低い周波数（ぼかした像）は縁からの距離に比例する重みで広く混ぜ、
    // 高い周波数（元の像 − ぼかした像）は距離の kHighBandPower 乗の重みで混ぜる。
    // 重みの合計を先に数えておき（1回目）、2回目で割りながら足す。こうすると
    // 出力のほかに持つのは重み2面だけで済む。
    const std::size_t out_pixels = static_cast<std::size_t>(out_w) * out_h;
    std::vector<float> low_total(out_pixels, 0.0f), high_total(out_pixels, 0.0f);
    FrameBuffer out(out_w, out_h, channels);
    const LanczosTable table;

    struct Box {
        int x0, y0, x1, y1;
    };
    std::vector<Box> boxes(static_cast<std::size_t>(count));
    int placed_total = 0;
    for (int i = 0; i < count; ++i) {
        if (!member[static_cast<std::size_t>(i)]) continue;
        ++placed_total;
        const Panel& p = panels[static_cast<std::size_t>(i)];
        double bx0 = 1e30, by0 = 1e30, bx1 = -1e30, by1 = -1e30;
        const double corners[4][2] = {{0.0, 0.0},
                                      {static_cast<double>(p.w - 1), 0.0},
                                      {0.0, static_cast<double>(p.h - 1)},
                                      {static_cast<double>(p.w - 1), static_cast<double>(p.h - 1)}};
        for (const auto& c : corners) {
            double X, Y;
            transforms[static_cast<std::size_t>(i)].apply(c[0], c[1], X, Y);
            bx0 = std::min(bx0, X);
            by0 = std::min(by0, Y);
            bx1 = std::max(bx1, X);
            by1 = std::max(by1, Y);
        }
        Box& box = boxes[static_cast<std::size_t>(i)];
        box.x0 = std::max(0, static_cast<int>(std::floor(bx0)));
        box.y0 = std::max(0, static_cast<int>(std::floor(by0)));
        box.x1 = std::min(out_w, static_cast<int>(std::ceil(bx1)) + 1);
        box.y1 = std::min(out_h, static_cast<int>(std::ceil(by1)) + 1);
    }

    // パネルの画素 (u, v) での重み。パネルの外・値のない画素（真っ黒）なら false。
    const auto weights_at = [&lumas](int index, const Panel& p, double u, double v, float& wl,
                                     float& wh) {
        const double eps = 1e-6;
        if (u < -eps || v < -eps || u > p.w - 1 + eps || v > p.h - 1 + eps) return false;
        const int iu = clampi(static_cast<int>(std::lround(u)), 0, p.w - 1);
        const int iv = clampi(static_cast<int>(std::lround(v)), 0, p.h - 1);
        if (!(lumas[static_cast<std::size_t>(index)].row(iv)[iu] > 0.0f)) return false;
        const double d = std::min(std::min(u, p.w - 1 - u), std::min(v, p.h - 1 - v)) + 0.5;
        const double dn = std::max(1e-4, d / (0.5 * std::min(p.w, p.h)));
        wl = static_cast<float>(dn);
        double high = dn;
        for (int k = 1; k < kHighBandPower; ++k) high *= dn;
        wh = static_cast<float>(std::max(high, 1e-30));
        return true;
    };

    int step = 0;
    const int steps = placed_total * 2;
    for (int i = 0; i < count; ++i) {
        if (!member[static_cast<std::size_t>(i)]) continue;
        const Panel& p = panels[static_cast<std::size_t>(i)];
        const Similarity t = transforms[static_cast<std::size_t>(i)];
        const Box box = boxes[static_cast<std::size_t>(i)];
        detail::parallel_rows(box.y1 - box.y0, [&](int r0, int r1) {
            for (int Y = box.y0 + r0; Y < box.y0 + r1; ++Y) {
                float* lt = low_total.data() + static_cast<std::size_t>(Y) * out_w;
                float* ht = high_total.data() + static_cast<std::size_t>(Y) * out_w;
                for (int X = box.x0; X < box.x1; ++X) {
                    double u, v;
                    t.invert(X, Y, u, v);
                    float wl, wh;
                    if (!weights_at(i, p, u, v, wl, wh)) continue;
                    lt[X] += wl;
                    ht[X] += wh;
                }
            }
        });
        notify(progress, "モザイク: 合成", ++step, steps);
    }

    for (int i = 0; i < count; ++i) {
        if (!member[static_cast<std::size_t>(i)]) continue;
        const Panel& p = panels[static_cast<std::size_t>(i)];
        const FrameBuffer& frame = *p.frame;
        const Similarity t = transforms[static_cast<std::size_t>(i)];
        const Box box = boxes[static_cast<std::size_t>(i)];
        const float gain = static_cast<float>(gains[static_cast<std::size_t>(i)]);
        // 倍率は背景より上にだけ掛け、背景は全パネル共通の値へそろえる（低い周波数の側で扱う）。
        float own_background[3], common_background[3];
        for (int c = 0; c < 3; ++c) {
            own_background[c] = static_cast<float>(backgrounds[static_cast<std::size_t>(i)].channel[c]);
            common_background[c] = static_cast<float>(common.channel[c]);
        }

        // ぼかした像（低い周波数）。
        const int radius = std::max(3, std::min(32, static_cast<int>(std::lround(std::min(p.w, p.h) / 40.0))));
        FrameBuffer low(p.w, p.h, frame.channels());
        for (int c = 0; c < frame.channels(); ++c) {
            for (int y = 0; y < p.h; ++y) {
                std::memcpy(low.row(c, y), frame.row(c, y), sizeof(float) * static_cast<std::size_t>(p.w));
            }
            box_blur3(low.plane(c), p.w, p.h, low.stride(), radius);
        }

        detail::parallel_rows(box.y1 - box.y0, [&](int r0, int r1) {
            for (int Y = box.y0 + r0; Y < box.y0 + r1; ++Y) {
                const float* lt = low_total.data() + static_cast<std::size_t>(Y) * out_w;
                const float* ht = high_total.data() + static_cast<std::size_t>(Y) * out_w;
                for (int X = box.x0; X < box.x1; ++X) {
                    double u, v;
                    t.invert(X, Y, u, v);
                    float wl, wh;
                    if (!weights_at(i, p, u, v, wl, wh)) continue;
                    const float share_low = wl / lt[X];
                    const float share_high = wh / ht[X];
                    for (int c = 0; c < channels; ++c) {
                        const int sc = frame.channels() == 1 ? 0 : c;
                        const float full = sample_lanczos(frame.plane(sc), p.w, p.h, frame.stride(), u, v, table);
                        const float blurred = sample_bilinear(low.plane(sc), p.w, p.h, low.stride(), u, v);
                        // 低域 × 広い重み ＋ 高域（元 − 低域）× 鋭い重み
                        out.row(c, Y)[X] += share_low * (common_background[c] + gain * (blurred - own_background[c])) +
                                            share_high * gain * (full - blurred);
                    }
                }
            }
        });
        notify(progress, "モザイク: 合成", ++step, steps);
    }

    detail::parallel_rows(out_h, [&](int y0, int y1) {
        for (int c = 0; c < channels; ++c) {
            for (int y = y0; y < y1; ++y) {
                float* row = out.row(c, y);
                for (int x = 0; x < out_w; ++x) row[x] = row[x] < 0.0f ? 0.0f : (row[x] > 1.0f ? 1.0f : row[x]);
            }
        }
    });
    out.invalidate_luma();
    int depth = 0;
    for (int i = 0; i < count; ++i) {
        if (member[static_cast<std::size_t>(i)]) depth = std::max(depth, inputs[static_cast<std::size_t>(i)]->source_bit_depth());
    }
    out.set_source_bit_depth(depth > 0 ? depth : 16);

    // ---- 報告 ---------------------------------------------------------------
    report.width = out_w;
    report.height = out_h;
    report.placed = placed_total;
    report.used_rotation = use_rotation;
    report.background = common.found ? common.luma : 0.0;
    report.panels.assign(static_cast<std::size_t>(count), MosaicPlacement());
    double residual = 0.0;
    long long points = 0;
    for (const Link& link : links) {
        if (!member[static_cast<std::size_t>(link.i)] || !member[static_cast<std::size_t>(link.j)]) continue;
        const double rms = link_rms(link, transforms);
        residual += rms * rms * static_cast<double>(link.matches.size());
        points += static_cast<long long>(link.matches.size());
    }
    report.residual_rms = points > 0 ? std::sqrt(residual / static_cast<double>(points)) : 0.0;
    report.correspondences = static_cast<int>(points);
    for (int i = 0; i < count; ++i) {
        MosaicPlacement& out_p = report.panels[static_cast<std::size_t>(i)];
        out_p.placed = member[static_cast<std::size_t>(i)];
        if (!out_p.placed) continue;
        const Similarity& t = transforms[static_cast<std::size_t>(i)];
        out_p.a = t.a;
        out_p.b = t.b;
        out_p.tx = t.tx;
        out_p.ty = t.ty;
        out_p.gain = gains[static_cast<std::size_t>(i)];
        out_p.links = link_count[static_cast<std::size_t>(i)];
        report.max_rotation_degrees =
            std::max(report.max_rotation_degrees, std::fabs(std::atan2(t.b, t.a)) * 180.0 / M_PI);
    }
    return out;
}

}  // namespace stackcore
