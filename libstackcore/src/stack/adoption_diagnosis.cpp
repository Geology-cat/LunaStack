#include "stackcore/adoption_diagnosis.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>

namespace stackcore {
namespace {

typedef std::complex<double> Complex;

// 試す採用率（%）。
const double kPercents[] = {5.0, 10.0, 15.0, 25.0, 35.0, 50.0, 75.0, 100.0};

// 2組それぞれに最低これだけのフレームが要る。
const int kMinFramesPerHalf = 8;
// 1組のスタックで、APあたり最低これだけ使う採用率から測る（1枚では比較にならない）。
const int kMinKeepPerHalf = 2;
// 升目の数がこれに満たなければ、判断しない。
const int kMinTiles = 4;
// 最大の点数からこの割合以内なら「ほぼ同じ」とみなし、少ない採用率のほうを勧める。
const double kNearBest = 0.02;

// 基数2のFFT（その場）。升目は小さく数も少ないので、速さより移植性と決定論性を取る。
void fft_line(Complex* a, int n, int stride, const std::vector<Complex>& roots) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[static_cast<std::size_t>(i) * stride], a[static_cast<std::size_t>(j) * stride]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < len / 2; ++k) {
                const Complex w = roots[static_cast<std::size_t>(k) * step];
                Complex& lo = a[static_cast<std::size_t>(i + k) * stride];
                Complex& hi = a[static_cast<std::size_t>(i + k + len / 2) * stride];
                const Complex u = lo, v = hi * w;
                lo = u + v;
                hi = u - v;
            }
        }
    }
}

void fft_square(std::vector<Complex>& a, int n, const std::vector<Complex>& roots) {
    for (int y = 0; y < n; ++y) fft_line(&a[static_cast<std::size_t>(y) * n], n, 1, roots);
    for (int x = 0; x < n; ++x) fft_line(&a[static_cast<std::size_t>(x)], n, n, roots);
}

// どのAPの重みも満たしている画素（4つのAPに覆われている）を数えるための面。
// 50%オーバーラップの格子では、内側の画素はちょうど4つのAPに入り、Hann窓の重みの合計が1になる。
// それより少ない画素は参照画像と混ざる（2組に同じ画像が入り、相関を水増しする）ので使わない。
std::vector<std::uint8_t> coverage_map(const AnalysisData& analysis) {
    const int w = analysis.width, h = analysis.height;
    std::vector<std::uint8_t> count(static_cast<std::size_t>(w) * h, 0);
    const int half = analysis.ap_size / 2;
    for (const AlignmentPoint& p : analysis.points) {
        const int x0 = std::max(0, p.cx - half), x1 = std::min(w, p.cx - half + analysis.ap_size);
        const int y0 = std::max(0, p.cy - half), y1 = std::min(h, p.cy - half + analysis.ap_size);
        for (int y = y0; y < y1; ++y) {
            std::uint8_t* row = count.data() + static_cast<std::size_t>(y) * w;
            for (int x = x0; x < x1; ++x) {
                if (row[x] < 255) ++row[x];
            }
        }
    }
    return count;
}

struct Tile {
    int x = 0;
    int y = 0;
};

// APに完全に覆われた升目を並べる（y 昇順 → x 昇順）。
std::vector<Tile> place_tiles(const std::vector<std::uint8_t>& coverage, int w, int h, int n) {
    std::vector<Tile> tiles;
    const int needed = n * n - n * n / 50;  // 98%
    for (int y = 0; y + n <= h; y += n / 2) {
        for (int x = 0; x + n <= w; x += n / 2) {
            int full = 0;
            for (int v = 0; v < n; ++v) {
                const std::uint8_t* row = coverage.data() + static_cast<std::size_t>(y + v) * w + x;
                for (int u = 0; u < n; ++u) {
                    if (row[u] >= 4) ++full;
                }
            }
            if (full >= needed) {
                Tile t;
                t.x = x;
                t.y = y;
                tiles.push_back(t);
            }
        }
    }
    return tiles;
}

// 解析結果から、analyzed の偶数番目・奇数番目だけを抜き出す。
AnalysisData half_of(const AnalysisData& analysis, int parity) {
    AnalysisData out;
    out.source_size = analysis.source_size;
    out.source_frames = analysis.source_frames;
    out.width = analysis.width;
    out.height = analysis.height;
    out.channels = analysis.channels;
    out.frames = analysis.frames;
    out.reference_index = analysis.reference_index;
    out.reference_mean = analysis.reference_mean;
    out.ap_size = analysis.ap_size;
    out.ap_grid_step = analysis.ap_grid_step;
    out.points = analysis.points;
    out.reference = analysis.reference;

    const std::size_t frame_count = analysis.analyzed_indices.size();
    const std::size_t ap_count = analysis.points.size();
    std::vector<std::size_t> picked;
    for (std::size_t i = static_cast<std::size_t>(parity); i < frame_count; i += 2) picked.push_back(i);
    out.analyzed_indices.reserve(picked.size());
    for (std::size_t i : picked) out.analyzed_indices.push_back(analysis.analyzed_indices[i]);
    out.matrix.resize(ap_count * picked.size());
    for (std::size_t a = 0; a < ap_count; ++a) {
        for (std::size_t k = 0; k < picked.size(); ++k) {
            out.matrix[a * picked.size() + k] = analysis.matrix[a * frame_count + picked[k]];
        }
    }
    return out;
}

// 2枚の画像の、升目にわたるフーリエ環相関。ring[r] は半径 r の環（r = 1 .. n/2-1）。
void ring_correlation(const FrameBuffer& a, const FrameBuffer& b, const std::vector<Tile>& tiles,
                      int n, std::vector<double>& ring) {
    std::vector<Complex> roots(static_cast<std::size_t>(n) / 2);
    for (int k = 0; k < n / 2; ++k) {
        const double angle = -2.0 * M_PI * k / n;
        roots[static_cast<std::size_t>(k)] = Complex(std::cos(angle), std::sin(angle));
    }
    std::vector<double> window(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) window[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * M_PI * (i + 0.5) / n);

    std::vector<double> cross(static_cast<std::size_t>(n) / 2, 0.0), power_a(cross.size(), 0.0),
        power_b(cross.size(), 0.0);
    std::vector<Complex> fa(static_cast<std::size_t>(n) * n), fb(fa.size());
    const float* la = a.luma();
    const float* lb = b.luma();
    for (const Tile& t : tiles) {
        double mean_a = 0.0, mean_b = 0.0;
        for (int v = 0; v < n; ++v) {
            const float* ra = la + static_cast<std::size_t>(t.y + v) * a.stride() + t.x;
            const float* rb = lb + static_cast<std::size_t>(t.y + v) * b.stride() + t.x;
            for (int u = 0; u < n; ++u) {
                mean_a += ra[u];
                mean_b += rb[u];
            }
        }
        mean_a /= static_cast<double>(n) * n;
        mean_b /= static_cast<double>(n) * n;
        for (int v = 0; v < n; ++v) {
            const float* ra = la + static_cast<std::size_t>(t.y + v) * a.stride() + t.x;
            const float* rb = lb + static_cast<std::size_t>(t.y + v) * b.stride() + t.x;
            for (int u = 0; u < n; ++u) {
                const double wgt = window[static_cast<std::size_t>(v)] * window[static_cast<std::size_t>(u)];
                fa[static_cast<std::size_t>(v) * n + u] = (ra[u] - mean_a) * wgt;
                fb[static_cast<std::size_t>(v) * n + u] = (rb[u] - mean_b) * wgt;
            }
        }
        fft_square(fa, n, roots);
        fft_square(fb, n, roots);
        for (int ky = 0; ky < n; ++ky) {
            const int fy = ky < n / 2 ? ky : ky - n;
            for (int kx = 0; kx < n; ++kx) {
                const int fx = kx < n / 2 ? kx : kx - n;
                const int r = static_cast<int>(std::lround(std::hypot(fx, fy)));
                if (r < 1 || r >= n / 2) continue;
                const Complex& x = fa[static_cast<std::size_t>(ky) * n + kx];
                const Complex& y = fb[static_cast<std::size_t>(ky) * n + kx];
                cross[static_cast<std::size_t>(r)] += x.real() * y.real() + x.imag() * y.imag();
                power_a[static_cast<std::size_t>(r)] += std::norm(x);
                power_b[static_cast<std::size_t>(r)] += std::norm(y);
            }
        }
    }
    ring.assign(cross.size(), 0.0);
    for (std::size_t r = 1; r < cross.size(); ++r) {
        const double denom = std::sqrt(power_a[r] * power_b[r]);
        ring[r] = denom > 0.0 ? cross[r] / denom : 0.0;
    }
}

}  // namespace

AdoptionDiagnosis diagnose_adoption(const VideoSource& source, const MapStackSettings& settings,
                                    const AnalysisData& analysis, const ProgressFn& progress) {
    AdoptionDiagnosis out;
    const int frame_count = static_cast<int>(analysis.analyzed_indices.size());
    out.frames = frame_count;
    if (analysis.points.empty() || analysis.reference.empty() || frame_count < 2 * kMinFramesPerHalf) {
        out.insufficient = true;
        return out;
    }

    // 升目。大きいほうが周波数を細かく分けられる。覆われた範囲が狭ければ小さくする。
    const std::vector<std::uint8_t> coverage = coverage_map(analysis);
    std::vector<Tile> tiles;
    for (int n : {64, 32}) {
        tiles = place_tiles(coverage, analysis.width, analysis.height, n);
        out.tile_size = n;
        if (static_cast<int>(tiles.size()) >= (n == 64 ? 2 * kMinTiles : kMinTiles)) break;
    }
    out.tiles = static_cast<int>(tiles.size());
    if (out.tiles < kMinTiles) {
        out.insufficient = true;
        return out;
    }

    const AnalysisData halves[2] = {half_of(analysis, 0), half_of(analysis, 1)};
    const int half_frames = static_cast<int>(halves[1].analyzed_indices.size());  // 少ないほう

    // 試す採用率。1組での採用枚数が同じになるものは、1つにまとめる。
    struct Candidate {
        double percent;
        int keep;
    };
    std::vector<Candidate> candidates;
    for (double percent : kPercents) {
        const int keep = std::min(half_frames, static_cast<int>(half_frames * percent / 100.0 + 0.5));
        if (keep < kMinKeepPerHalf) continue;
        if (!candidates.empty() && candidates.back().keep == keep) continue;
        Candidate c;
        c.percent = percent;
        c.keep = keep;
        candidates.push_back(c);
    }
    if (candidates.size() < 2) {
        out.insufficient = true;
        return out;
    }

    // 進み具合は「読むフレームの数」で出す（多く使う採用率ほど時間がかかる）。
    long long total_work = 0;
    for (const Candidate& c : candidates) total_work += 2LL * c.keep;
    long long done_work = 0;
    int last_reported = -1;  // 同じ値を何度も通知しない

    MapStackSettings stack_settings = settings;
    stack_settings.drizzle_scale = 1.0;
    stack_settings.pixfrac = 1.0;
    stack_settings.ap_top_count = 0;

    const int n = out.tile_size;
    for (const Candidate& c : candidates) {
        stack_settings.ap_top_percent = c.percent;
        FrameBuffer stacked[2];
        for (int k = 0; k < 2; ++k) {
            const long long base = done_work;
            const long long slice = c.keep;
            const ProgressFn inner = [&](const char*, int done, int total) -> bool {
                if (!progress) return true;
                const long long now = base + (total > 0 ? slice * done / total : 0);
                const int permille = static_cast<int>(now * 1000 / std::max(1LL, total_work));
                if (permille == last_reported) return true;
                last_reported = permille;
                return progress("採用率の目安", permille, 1000);
            };
            MapStackReport report;
            stacked[k] = stack_from_analysis(source, stack_settings, halves[k], inner, report);
            done_work += slice;
        }

        std::vector<double> ring;
        ring_correlation(stacked[0], stacked[1], tiles, n, ring);

        AdoptionPoint point;
        point.percent = c.percent;
        point.frames_per_ap = std::max(1, std::min(frame_count, static_cast<int>(frame_count * c.percent / 100.0 + 0.5)));
        // 半分の枚数どうしの相関 r を、全フレームを使ったときの値 2r/(1+r) に換算して平均する。
        double sum = 0.0;
        int rings = 0;
        double previous = 1.0;
        bool crossed = false;
        for (std::size_t r = 1; r < ring.size(); ++r) {
            const double half = std::max(0.0, std::min(1.0, ring[r]));
            const double full = 2.0 * half / (1.0 + half);
            sum += full;
            ++rings;
            if (!crossed && full < 0.5) {
                // 直前の環との間を直線でつないで、0.5 を横切る周波数を求める。
                const double t = previous > full ? (previous - 0.5) / (previous - full) : 0.0;
                point.cutoff = (static_cast<double>(r) - 1.0 + t) / n;
                crossed = true;
            }
            previous = full;
        }
        if (!crossed) point.cutoff = 0.5;
        point.score = rings > 0 ? sum / rings : 0.0;
        out.points.push_back(point);
    }
    if (progress && !progress("採用率の目安", 1000, 1000)) throw Cancelled();

    double best = -1.0;
    for (const AdoptionPoint& p : out.points) {
        if (p.score > best) {
            best = p.score;
            out.best_percent = p.percent;
        }
    }
    if (!(best > 0.0)) {
        out.insufficient = true;  // 再現する模様が無い
        return out;
    }
    for (const AdoptionPoint& p : out.points) {
        if (p.score >= best * (1.0 - kNearBest)) {
            out.suggested_percent = p.percent;
            break;
        }
    }
    return out;
}

}  // namespace stackcore
