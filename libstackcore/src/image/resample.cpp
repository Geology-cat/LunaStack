#include "stackcore/resample.hpp"

#include <cmath>
#include <vector>

namespace stackcore {
namespace {

constexpr int kTaps = 6;   // Lanczos3 は片側3タップ、合計6
constexpr int kRadius = 3;

int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

double lanczos3_kernel(double x) {
    if (x == 0.0) return 1.0;
    const double ax = x < 0.0 ? -x : x;
    if (ax >= 3.0) return 0.0;
    const double px = M_PI * x;
    return 3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px);
}

void copy_patch_clamped(const float* src, int src_width, int src_height,
                        std::size_t src_stride, int src_x0, int src_y0, float* dst,
                        int dst_width, int dst_height, std::size_t dst_stride) {
    for (int oy = 0; oy < dst_height; ++oy) {
        const int sy = clamp_int(src_y0 + oy, 0, src_height - 1);
        const float* s = src + static_cast<std::size_t>(sy) * src_stride;
        float* d = dst + static_cast<std::size_t>(oy) * dst_stride;
        for (int ox = 0; ox < dst_width; ++ox) {
            d[ox] = s[clamp_int(src_x0 + ox, 0, src_width - 1)];
        }
    }
}

void resample_lanczos3(const float* src, int src_width, int src_height,
                       std::size_t src_stride, double src_x0, double src_y0, float* dst,
                       int dst_width, int dst_height, std::size_t dst_stride) {
    if (dst_width <= 0 || dst_height <= 0) return;

    // 整数部と小数部に分ける。小数部は 0 <= f < 1 に正規化する。
    const double fx0 = std::floor(src_x0);
    const double fy0 = std::floor(src_y0);
    const double frac_x = src_x0 - fx0;
    const double frac_y = src_y0 - fy0;
    const int ix0 = static_cast<int>(fx0);
    const int iy0 = static_cast<int>(fy0);

    if (frac_x == 0.0 && frac_y == 0.0) {
        copy_patch_clamped(src, src_width, src_height, src_stride, ix0, iy0, dst, dst_width,
                           dst_height, dst_stride);
        return;
    }

    // 出力画素の位置は入力格子に対して一定の小数オフセットを持つので、
    // 重みは x, y それぞれ1組を使い回せる。
    //
    // 重みは合計が1になるよう正規化する。Lanczos3のタップ和は厳密には1にならず、
    // 正規化しないと平坦な領域でもわずかな明暗ムラが出る。
    // オーバーラップ窓合成では、それが格子状のアーティファクトとして見える。
    double wx[kTaps], wy[kTaps];
    double sum_x = 0.0, sum_y = 0.0;
    for (int t = 0; t < kTaps; ++t) {
        const double dx = static_cast<double>(t - (kRadius - 1)) - frac_x;
        const double dy = static_cast<double>(t - (kRadius - 1)) - frac_y;
        wx[t] = lanczos3_kernel(dx);
        wy[t] = lanczos3_kernel(dy);
        sum_x += wx[t];
        sum_y += wy[t];
    }
    if (sum_x != 0.0) {
        for (int t = 0; t < kTaps; ++t) wx[t] /= sum_x;
    }
    if (sum_y != 0.0) {
        for (int t = 0; t < kTaps; ++t) wy[t] /= sum_y;
    }

    // 横方向を先に掛けた中間結果を持つ（分離可能フィルタ）。
    // 縦のタップぶん余分に作る必要があるので高さは dst_height + kTaps - 1。
    const int tmp_height = dst_height + kTaps - 1;
    std::vector<float> tmp(static_cast<std::size_t>(dst_width) * tmp_height);

    for (int ty = 0; ty < tmp_height; ++ty) {
        const int sy = clamp_int(iy0 + ty - (kRadius - 1), 0, src_height - 1);
        const float* s = src + static_cast<std::size_t>(sy) * src_stride;
        float* t = tmp.data() + static_cast<std::size_t>(ty) * dst_width;
        for (int ox = 0; ox < dst_width; ++ox) {
            const int base = ix0 + ox - (kRadius - 1);
            double acc = 0.0;
            for (int k = 0; k < kTaps; ++k) {
                acc += wx[k] * s[clamp_int(base + k, 0, src_width - 1)];
            }
            t[ox] = static_cast<float>(acc);
        }
    }

    for (int oy = 0; oy < dst_height; ++oy) {
        float* d = dst + static_cast<std::size_t>(oy) * dst_stride;
        for (int ox = 0; ox < dst_width; ++ox) {
            double acc = 0.0;
            for (int k = 0; k < kTaps; ++k) {
                acc += wy[k] * tmp[static_cast<std::size_t>(oy + k) * dst_width + ox];
            }
            d[ox] = static_cast<float>(acc);
        }
    }
}

}  // namespace stackcore
