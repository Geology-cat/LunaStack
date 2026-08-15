#include "stackcore/ap_placer.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "stackcore/quality.hpp"

namespace stackcore {
namespace {

// 仕様書 §4.5 のAPサイズ選択肢。
const int kApSizes[] = {32, 48, 64, 96, 128, 200};

// 画素ごとの勾配の大きさ |∇I| を作る。
// 品質評価（勾配エネルギー Σ|∇I|²）と違い、AP採否はエネルギーではなく
// 平均勾配 T = (1/N)Σ|∇I| で判断する（仕様書 §4.5）。
// 2乗しないぶん、少数の強い縁より「面として模様があるか」を見ることになる。
void gradient_components(const FrameBuffer& frame, std::vector<float>& mag,
                         std::vector<float>& gxx, std::vector<float>& gyy,
                         std::vector<float>& gxy, QualityWorkspace& ws) {
    const int w = frame.width(), h = frame.height();
    const std::size_t stride = frame.stride();
    ws.ensure(stride * static_cast<std::size_t>(h));
    gaussian_blur_5tap(frame.luma(), ws.blurred.data(), w, h, stride, ws.scratch.data());
    const float* b = ws.blurred.data();

    const std::size_t n = static_cast<std::size_t>(w) * h;
    mag.assign(n, 0.0f);
    gxx.assign(n, 0.0f);
    gyy.assign(n, 0.0f);
    gxy.assign(n, 0.0f);

    for (int y = 1; y < h - 1; ++y) {
        const float* prev = b + static_cast<std::size_t>(y - 1) * stride;
        const float* cur = b + static_cast<std::size_t>(y) * stride;
        const float* next = b + static_cast<std::size_t>(y + 1) * stride;
        const std::size_t row = static_cast<std::size_t>(y) * w;
        for (int x = 1; x < w - 1; ++x) {
            const double gx = 0.5 * (static_cast<double>(cur[x + 1]) - cur[x - 1]);
            const double gy = 0.5 * (static_cast<double>(next[x]) - prev[x]);
            mag[row + x] = static_cast<float>(std::sqrt(gx * gx + gy * gy));
            gxx[row + x] = static_cast<float>(gx * gx);
            gyy[row + x] = static_cast<float>(gy * gy);
            gxy[row + x] = static_cast<float>(gx * gy);
        }
    }
}

// 積分画像。矩形和を4点の引き算で取れるようにする。
// APは50%オーバーラップで敷き詰めるため、素朴に領域ごとに足すと
// 同じ画素を4回ずつ舐めることになる。
void build_integral(const std::vector<float>& src, int w, int h, std::vector<double>& out) {
    out.assign(static_cast<std::size_t>(w + 1) * (h + 1), 0.0);
    for (int y = 0; y < h; ++y) {
        double row_sum = 0.0;
        for (int x = 0; x < w; ++x) {
            row_sum += src[static_cast<std::size_t>(y) * w + x];
            out[static_cast<std::size_t>(y + 1) * (w + 1) + (x + 1)] =
                out[static_cast<std::size_t>(y) * (w + 1) + (x + 1)] + row_sum;
        }
    }
}

double rect_sum(const std::vector<double>& integral, int w, int x0, int y0, int x1, int y1) {
    const std::size_t stride = static_cast<std::size_t>(w + 1);
    return integral[static_cast<std::size_t>(y1) * stride + x1] -
           integral[static_cast<std::size_t>(y0) * stride + x1] -
           integral[static_cast<std::size_t>(y1) * stride + x0] +
           integral[static_cast<std::size_t>(y0) * stride + x0];
}

}  // namespace

int suggest_ap_size(const FrameBuffer& reference) {
    if (reference.empty()) return 64;
    const int w = reference.width(), h = reference.height();
    const std::size_t stride = reference.stride();
    const float* luma = reference.luma();

    // 明るい領域の広がりを見る。惑星なら円盤の直径、月面なら視野全体になる。
    float hi = 0.0f;
    for (int y = 0; y < h; ++y) {
        const float* row = luma + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            if (row[x] > hi) hi = row[x];
        }
    }
    const float threshold = hi * 0.25f;

    int min_x = w, max_x = -1, min_y = h, max_y = -1;
    for (int y = 0; y < h; ++y) {
        const float* row = luma + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            if (row[x] <= threshold) continue;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }
    if (max_x < min_x || max_y < min_y) return 64;

    const int span_x = max_x - min_x + 1;
    const int span_y = max_y - min_y + 1;
    const int shorter = span_x < span_y ? span_x : span_y;

    // 対象を6分割できる程度を目安にする。細かすぎると相関が立たず、
    // 粗すぎるとシーイングの局所性を追えない。
    const int target = shorter / 6;
    int best = kApSizes[0];
    int best_diff = -1;
    for (int s : kApSizes) {
        const int d = s > target ? s - target : target - s;
        if (best_diff < 0 || d < best_diff) {
            best_diff = d;
            best = s;
        }
    }
    return best;
}

std::vector<AlignmentPoint> place_alignment_points(const FrameBuffer& reference,
                                                   const ApPlacementSettings& settings,
                                                   int& used_ap_size) {
    if (reference.empty()) throw std::invalid_argument("AP配置: 参照フレームが空です");

    const int w = reference.width(), h = reference.height();
    const int ap = settings.ap_size > 0 ? settings.ap_size : suggest_ap_size(reference);
    used_ap_size = ap;
    if (ap < 8 || ap > w || ap > h) {
        throw std::invalid_argument("AP配置: APサイズが画像に対して不適切です (" +
                                    std::to_string(ap) + ")");
    }

    QualityWorkspace ws;
    std::vector<float> grad, gxx, gyy, gxy;
    gradient_components(reference, grad, gxx, gyy, gxy, ws);

    // 輝度も同じ積分画像の枠組みで扱う。
    std::vector<float> level(static_cast<std::size_t>(w) * h);
    const std::size_t stride = reference.stride();
    const float* luma = reference.luma();
    float max_level = 0.0f;
    for (int y = 0; y < h; ++y) {
        const float* row = luma + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            level[static_cast<std::size_t>(y) * w + x] = row[x];
            if (row[x] > max_level) max_level = row[x];
        }
    }

    std::vector<double> grad_integral, level_integral, xx_integral, yy_integral, xy_integral;
    build_integral(grad, w, h, grad_integral);
    build_integral(level, w, h, level_integral);
    build_integral(gxx, w, h, xx_integral);
    build_integral(gyy, w, h, yy_integral);
    build_integral(gxy, w, h, xy_integral);

    // 構造テンソルの最小固有値。
    // [[Sxx, Sxy], [Sxy, Syy]] の固有値は (tr ± sqrt(tr² - 4det)) / 2。
    auto min_eigen = [&](int x0, int y0, int x1, int y1, double area) {
        const double sxx = rect_sum(xx_integral, w, x0, y0, x1, y1) / area;
        const double syy = rect_sum(yy_integral, w, x0, y0, x1, y1) / area;
        const double sxy = rect_sum(xy_integral, w, x0, y0, x1, y1) / area;
        const double tr = sxx + syy;
        const double diff = sxx - syy;
        const double root = std::sqrt(diff * diff + 4.0 * sxy * sxy);
        return 0.5 * (tr - root);
    };

    // 画像全体の平均勾配を基準にする。絶対値の閾値だと露出やビット深度で
    // 意味が変わってしまう。
    const double whole_mean_gradient =
        rect_sum(grad_integral, w, 0, 0, w, h) / (static_cast<double>(w) * h);
    const double gradient_threshold = whole_mean_gradient * settings.gradient_ratio;
    const double level_threshold = static_cast<double>(max_level) * settings.level_ratio;

    // 構造の強さの基準は、AP候補位置での最小固有値の中央値にする。
    // 画像全体の平均だと背景（ほぼ0）に引きずられて基準が緩くなりすぎる。
    const int step_pre = ap / 2;
    const int half_pre = ap / 2;
    std::vector<double> eigen_samples;
    for (int cy = half_pre; cy + half_pre <= h; cy += step_pre) {
        for (int cx = half_pre; cx + half_pre <= w; cx += step_pre) {
            const int x0 = cx - half_pre, y0 = cy - half_pre;
            const double area = static_cast<double>(ap) * ap;
            const double lvl = rect_sum(level_integral, w, x0, y0, x0 + ap, y0 + ap) / area;
            if (lvl < level_threshold) continue;  // 背景は基準づくりに入れない
            eigen_samples.push_back(min_eigen(x0, y0, x0 + ap, y0 + ap, area));
        }
    }
    double eigen_threshold = 0.0;
    if (!eigen_samples.empty()) {
        std::sort(eigen_samples.begin(), eigen_samples.end());
        eigen_threshold =
            eigen_samples[eigen_samples.size() / 2] * settings.min_structure_ratio;
    }

    // 配置間隔はAPサイズの1/2。50%オーバーラップは⑧の継ぎ目防止の前提条件。
    const int step = ap / 2;
    const int half = ap / 2;

    std::vector<AlignmentPoint> points;

    // 手動指定があればそれを使う。
    // 測定値（平均勾配など）は自動配置と同じ式で埋めておく。
    // 表示や診断で「この点は模様が乏しい」と示せるようにするためで、
    // 採否には使わない。
    if (settings.use_manual_points) {
        for (std::size_t i = 0; i < settings.manual_points.size(); ++i) {
            const int cx = settings.manual_points[i].cx;
            const int cy = settings.manual_points[i].cy;
            const int x0 = cx - half, y0 = cy - half;
            const int x1 = x0 + ap, y1 = y0 + ap;
            if (x0 < 0 || y0 < 0 || x1 > w || y1 > h) continue;

            const double area = static_cast<double>(ap) * ap;
            AlignmentPoint p;
            p.cx = cx;
            p.cy = cy;
            p.mean_gradient = rect_sum(grad_integral, w, x0, y0, x1, y1) / area;
            p.mean_level = rect_sum(level_integral, w, x0, y0, x1, y1) / area;
            p.min_eigenvalue = min_eigen(x0, y0, x1, y1, area);
            points.push_back(p);
        }
        // 順序は自動配置と揃えて y 昇順 → x 昇順に固定する（決定論性の要件）。
        // 利用者がクリックした順に並べると、同じAP集合でも
        // 追加した順番だけで加算順が変わってしまう。
        std::sort(points.begin(), points.end(),
                  [](const AlignmentPoint& a, const AlignmentPoint& b) {
                      if (a.cy != b.cy) return a.cy < b.cy;
                      return a.cx < b.cx;
                  });
        return points;
    }

    // 順序は y 昇順 → x 昇順に固定する（決定論性の要件）。
    for (int cy = half; cy + half <= h || settings.allow_partial_edge; cy += step) {
        if (cy >= h) break;
        for (int cx = half; cx + half <= w || settings.allow_partial_edge; cx += step) {
            if (cx >= w) break;

            int x0 = cx - half, y0 = cy - half;
            int x1 = x0 + ap, y1 = y0 + ap;
            if (x0 < 0 || y0 < 0 || x1 > w || y1 > h) {
                if (!settings.allow_partial_edge) continue;
                x0 = std::max(0, x0);
                y0 = std::max(0, y0);
                x1 = std::min(w, x1);
                y1 = std::min(h, y1);
                if (x1 - x0 < 8 || y1 - y0 < 8) continue;
            }

            const double area = static_cast<double>(x1 - x0) * (y1 - y0);
            const double mean_grad = rect_sum(grad_integral, w, x0, y0, x1, y1) / area;
            const double mean_level = rect_sum(level_integral, w, x0, y0, x1, y1) / area;

            if (mean_grad < gradient_threshold) continue;
            if (mean_level < level_threshold) continue;

            const double lambda = min_eigen(x0, y0, x1, y1, area);
            if (lambda < eigen_threshold) continue;

            AlignmentPoint p;
            p.cx = cx;
            p.cy = cy;
            p.mean_gradient = mean_grad;
            p.mean_level = mean_level;
            p.min_eigenvalue = lambda;
            points.push_back(p);
        }
    }
    return points;
}

}  // namespace stackcore
