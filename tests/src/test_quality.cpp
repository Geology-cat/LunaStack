#include <algorithm>
#include <cstdint>
#include <cmath>
#include <vector>

#include "microtest.hpp"
#include "stackcore/frame_buffer.hpp"
#include "stackcore/quality.hpp"

using stackcore::FrameBuffer;
using stackcore::QualityWorkspace;

namespace {

// 決定論的な擬似乱数。テストが環境のrandに依存しないようにする。
struct Lcg {
    std::uint32_t s;
    explicit Lcg(std::uint32_t seed) : s(seed) {}
    float next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>((s >> 8) & 0xFFFF) / 65535.0f;
    }
};

// 中央に円盤を置いた合成惑星。blur_passes を増やすとぼけて品質が下がる。
FrameBuffer make_disc(int w, int h, double radius, int blur_passes, std::uint32_t seed) {
    FrameBuffer fb(w, h, 1);
    Lcg rng(seed);
    const double cx = w / 2.0, cy = h / 2.0;
    for (int y = 0; y < h; ++y) {
        float* row = fb.row(0, y);
        for (int x = 0; x < w; ++x) {
            const double dx = x - cx, dy = y - cy;
            const double r = std::sqrt(dx * dx + dy * dy);
            // 縞模様を入れて、ぼかしで失われる細部を作る。
            const double band = 0.5 + 0.35 * std::sin(dy * 0.7);
            double v = r <= radius ? band : 0.02;
            v += 0.01 * (rng.next() - 0.5);  // 現実的な微小ノイズ
            row[x] = static_cast<float>(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
        }
    }
    fb.invalidate_luma();

    if (blur_passes > 0) {
        std::vector<float> tmp(fb.stride() * static_cast<std::size_t>(h));
        std::vector<float> scratch(fb.stride() * static_cast<std::size_t>(h));
        for (int i = 0; i < blur_passes; ++i) {
            stackcore::gaussian_blur_5tap(fb.plane(0), tmp.data(), w, h, fb.stride(),
                                          scratch.data());
            std::copy(tmp.begin(), tmp.end(), fb.plane(0));
        }
        fb.invalidate_luma();
    }
    return fb;
}

}  // namespace

MT_TEST(quality_一様な画像の勾配エネルギーはゼロ) {
    FrameBuffer fb(32, 24, 1);
    for (int y = 0; y < 24; ++y) {
        float* row = fb.row(0, y);
        for (int x = 0; x < 32; ++x) row[x] = 0.5f;
    }
    fb.invalidate_luma();

    QualityWorkspace ws;
    MT_CHECK_NEAR(stackcore::gradient_energy(fb, ws), 0.0, 1e-12);
}

MT_TEST(quality_ぼけた画像ほどスコアが低い) {
    QualityWorkspace ws;
    const double sharp = stackcore::gradient_energy(make_disc(64, 64, 20.0, 0, 1), ws);
    const double mid = stackcore::gradient_energy(make_disc(64, 64, 20.0, 2, 1), ws);
    const double soft = stackcore::gradient_energy(make_disc(64, 64, 20.0, 6, 1), ws);

    if (!(sharp > mid && mid > soft)) {
        microtest::fail("ぼかすほどスコアが下がるはず: sharp=" + microtest::mt_str(sharp) +
                        " mid=" + microtest::mt_str(mid) + " soft=" + microtest::mt_str(soft));
    }
}

MT_TEST(quality_ぼかしは総和をほぼ保存する) {
    // ガウスぼかしの係数和は1。境界を値の複製で処理しているので、
    // 平坦な部分の値は変わらないはず。
    const int w = 40, h = 30;
    std::vector<float> src(static_cast<std::size_t>(w) * h, 0.25f);
    std::vector<float> dst(src.size(), -1.0f);
    std::vector<float> scratch(src.size());
    stackcore::gaussian_blur_5tap(src.data(), dst.data(), w, h,
                                  static_cast<std::size_t>(w), scratch.data());
    for (std::size_t i = 0; i < dst.size(); ++i) {
        MT_CHECK_NEAR(dst[i], 0.25f, 1e-6);
    }
}

MT_TEST(quality_ワークスペースを使い回しても結果が変わらない) {
    QualityWorkspace ws;
    const FrameBuffer a = make_disc(48, 48, 15.0, 0, 7);
    const double first = stackcore::gradient_energy(a, ws);
    // 別サイズを挟んでバッファを拡張させてから、もう一度同じ画像を測る。
    stackcore::gradient_energy(make_disc(96, 96, 30.0, 1, 9), ws);
    const double second = stackcore::gradient_energy(a, ws);
    MT_CHECK_NEAR(first, second, 1e-15);
}

MT_TEST(quality_周波数帯パワー比はぼけると低下する) {
    QualityWorkspace ws;
    const double sharp =
        stackcore::frequency_band_power_ratio(make_disc(64, 64, 20.0, 0, 11), ws);
    const double soft =
        stackcore::frequency_band_power_ratio(make_disc(64, 64, 20.0, 6, 11), ws);
    if (!(sharp > soft && sharp >= 0.0 && sharp <= 1.0 && soft >= 0.0 && soft <= 1.0)) {
        microtest::fail("周波数帯パワー比がぼけを識別できない: sharp=" +
                        microtest::mt_str(sharp) + " soft=" + microtest::mt_str(soft));
    }
}

MT_TEST(quality_周波数帯パワー比は決定論的で一様画像はゼロ) {
    FrameBuffer flat(64, 64, 1);
    for (int y = 0; y < flat.height(); ++y) {
        for (int x = 0; x < flat.width(); ++x) flat.row(0, y)[x] = 0.5f;
    }
    flat.invalidate_luma();

    QualityWorkspace ws;
    MT_CHECK_NEAR(stackcore::frequency_band_power_ratio(flat, ws), 0.0, 1e-12);
    const FrameBuffer image = make_disc(64, 64, 20.0, 1, 27);
    const double first = stackcore::quality_score(
        image, stackcore::QualityMetric::FrequencyBandPowerRatio, ws);
    const double second = stackcore::quality_score(
        image, stackcore::QualityMetric::FrequencyBandPowerRatio, ws);
    MT_CHECK_NEAR(first, second, 1e-15);
}
