#include <cmath>
#include <vector>

#include "microtest.hpp"
#include "stackcore/resample.hpp"

using stackcore::resample_lanczos3;

namespace {

// 連続関数として定義した合成シーン。
//
// **リサンプラで作った画像をリサンプラで測ってはいけない。**
// 自作リサンプラで歪めた画像を自作リサンプラで戻すと、カーネルの誤差が
// 部分的に打ち消し合い、精度が実際より良く見える
// （このプロジェクトが「自作writerとの自己一致」として警戒しているのと同じ罠）。
// そこで真値は常にこの連続関数から直接標本化して作る。
//
// 周期を8px以上にして帯域を抑えてある。ナイキストに近い成分を含めると、
// それはリサンプラの誤差ではなく標本化定理の限界を測ることになる。
double scene(double x, double y) {
    return 0.5 + 0.18 * std::sin(x * (2.0 * M_PI / 13.0)) *
                     std::cos(y * (2.0 * M_PI / 11.0)) +
           0.12 * std::sin((x + y) * (2.0 * M_PI / 17.0)) +
           0.06 * std::cos((x - 2.0 * y) * (2.0 * M_PI / 23.0));
}

std::vector<float> sample_scene(int w, int h, double x0, double y0) {
    std::vector<float> v(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            v[static_cast<std::size_t>(y) * w + x] =
                static_cast<float>(scene(x0 + x, y0 + y));
        }
    }
    return v;
}

// 端の影響を受けない内側だけで誤差を測る。
double inner_max_error(const std::vector<float>& got, const std::vector<float>& want, int w,
                       int h, int margin) {
    double worst = 0.0;
    for (int y = margin; y < h - margin; ++y) {
        for (int x = margin; x < w - margin; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            const double d = std::fabs(static_cast<double>(got[i]) - want[i]);
            if (d > worst) worst = d;
        }
    }
    return worst;
}

}  // namespace

MT_TEST(resample_カーネルは整数点で補間条件を満たす) {
    // Lanczos は sinc 由来なので、0で1、他の整数で0でなければならない。
    // ここが崩れると整数変位でも画像がぼける。
    MT_CHECK_NEAR(stackcore::lanczos3_kernel(0.0), 1.0, 1e-12);
    for (int k = 1; k <= 3; ++k) {
        MT_CHECK_NEAR(stackcore::lanczos3_kernel(k), 0.0, 1e-12);
        MT_CHECK_NEAR(stackcore::lanczos3_kernel(-k), 0.0, 1e-12);
    }
    MT_CHECK_NEAR(stackcore::lanczos3_kernel(3.5), 0.0, 1e-12);
}

MT_TEST(resample_整数変位は画素を変えない) {
    const int w = 40, h = 30;
    const std::vector<float> src = sample_scene(w, h, 0, 0);
    std::vector<float> dst(static_cast<std::size_t>(20) * 16, -1.0f);

    resample_lanczos3(src.data(), w, h, w, 5.0, 4.0, dst.data(), 20, 16, 20);

    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 20; ++x) {
            MT_CHECK_NEAR(dst[static_cast<std::size_t>(y) * 20 + x],
                          src[static_cast<std::size_t>(y + 4) * w + (x + 5)], 1e-6);
        }
    }
}

MT_TEST(resample_一様な画像は変位しても一様なまま) {
    // 重みの和が1でないと平坦な領域に明暗ムラが出る。
    // オーバーラップ窓合成ではそれが格子状のアーティファクトとして現れる。
    const int w = 32, h = 32;
    std::vector<float> src(static_cast<std::size_t>(w) * h, 0.375f);
    std::vector<float> dst(static_cast<std::size_t>(20) * 20, -1.0f);

    resample_lanczos3(src.data(), w, h, w, 5.37, 4.62, dst.data(), 20, 20, 20);
    for (std::size_t i = 0; i < dst.size(); ++i) MT_CHECK_NEAR(dst[i], 0.375f, 1e-6);
}

MT_TEST(resample_小数変位が連続関数の値と一致する) {
    // 真値は連続関数から直接標本化する（リサンプラを経由しない）。
    const int w = 64, h = 64;
    const std::vector<float> src = sample_scene(w, h, 0, 0);
    const int ow = 32, oh = 32;

    const double offsets[][2] = {{0.5, 0.5},   {0.25, 0.75}, {0.1, 0.9},
                                 {0.37, 0.11}, {0.99, 0.01}, {0.5, 0.0}};
    double worst = 0.0;
    for (const auto& off : offsets) {
        const double x0 = 12.0 + off[0];
        const double y0 = 14.0 + off[1];
        std::vector<float> dst(static_cast<std::size_t>(ow) * oh);
        resample_lanczos3(src.data(), w, h, w, x0, y0, dst.data(), ow, oh, ow);

        const std::vector<float> truth = sample_scene(ow, oh, x0, y0);
        const double e = inner_max_error(dst, truth, ow, oh, 0);
        if (e > worst) worst = e;
    }

    // 帯域を抑えた滑らかなシーンなら、Lanczos3は1e-3程度まで再現できる。
    if (!(worst < 2e-3)) {
        microtest::fail("小数変位の再現誤差が大きすぎる: " + microtest::mt_str(worst));
    }
}

MT_TEST(resample_境界では端の値を複製し暗くならない) {
    // 0埋めにすると縁が暗くなり、オーバーラップ窓合成で継ぎ目になる。
    const int w = 24, h = 24;
    std::vector<float> src(static_cast<std::size_t>(w) * h, 0.8f);
    std::vector<float> dst(static_cast<std::size_t>(8) * 8, -1.0f);

    // 左上の外側にはみ出す位置から切り出す。
    resample_lanczos3(src.data(), w, h, w, -2.4, -3.6, dst.data(), 8, 8, 8);
    for (std::size_t i = 0; i < dst.size(); ++i) {
        if (!(dst[i] > 0.75f)) {
            microtest::fail("境界で値が落ちている: " + microtest::mt_str(dst[i]));
            return;
        }
    }
}

MT_TEST(resample_strideが幅と異なるバッファでも正しく動く) {
    // FrameBuffer は32バイト境界に揃えるので stride != width が常態である。
    const int w = 40, h = 30;
    const std::size_t stride = 48;
    std::vector<float> src(stride * h, 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) src[stride * y + x] = static_cast<float>(scene(x, y));
    }

    const int ow = 16, oh = 12;
    const std::size_t dst_stride = 24;
    std::vector<float> dst(dst_stride * oh, -1.0f);
    resample_lanczos3(src.data(), w, h, stride, 8.5, 6.25, dst.data(), ow, oh, dst_stride);

    const std::vector<float> truth = sample_scene(ow, oh, 8.5, 6.25);
    for (int y = 0; y < oh; ++y) {
        for (int x = 0; x < ow; ++x) {
            MT_CHECK_NEAR(dst[dst_stride * y + x], truth[static_cast<std::size_t>(y) * ow + x],
                          2e-3);
        }
    }
}
