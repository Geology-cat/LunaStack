#include <cmath>
#include <cstdint>
#include <vector>

#include "microtest.hpp"
#include "stackcore/frame_buffer.hpp"
#include "stackcore/global_aligner.hpp"
#include "stackcore/quality.hpp"

using stackcore::AlignMode;
using stackcore::FrameBuffer;
using stackcore::GlobalAligner;
using stackcore::GlobalAlignResult;
using stackcore::GlobalAlignSettings;
using stackcore::RejectReason;

namespace {

struct Lcg {
    std::uint32_t s;
    explicit Lcg(std::uint32_t seed) : s(seed) {}
    float next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>((s >> 8) & 0xFFFF) / 65535.0f;
    }
};

// 暗背景に浮かぶ模様つきの円盤。惑星の代用。
FrameBuffer planet(int n, double cx, double cy, double radius, std::uint32_t seed,
                   double noise = 0.01) {
    FrameBuffer fb(n, n, 1);
    Lcg rng(seed);
    for (int y = 0; y < n; ++y) {
        float* row = fb.row(0, y);
        for (int x = 0; x < n; ++x) {
            const double dx = x - cx, dy = y - cy;
            const double r = std::sqrt(dx * dx + dy * dy);
            double v = r <= radius
                           ? 0.55 + 0.25 * std::sin(dy * 0.9) + 0.12 * std::cos(dx * 1.3)
                           : 0.02;
            v += noise * (rng.next() - 0.5);
            row[x] = static_cast<float>(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
        }
    }
    fb.invalidate_luma();
    return fb;
}

// 視野全面に模様がある画像。月・太陽の広視野の代用。
//
// 三角関数で作った模様は使わない。周期的な模様は位相相関に本質的な多義性を持ち
// （半周期ずらすと同じ見え方になる）、実装の正しさを測れないため。
// 白色ノイズを数回ぼかした非周期のテクスチャを1枚だけ作り、
// **同じ基底画像を別位置で切り出す**ことで、変位がちょうど整数になるようにする。
FrameBuffer wide_field(int n, int ox, int oy, std::uint32_t seed) {
    const int margin = 32;
    const int m = n + 2 * margin;
    const std::size_t total = static_cast<std::size_t>(m) * m;
    std::vector<float> base(total), tmp(total), scratch(total);

    Lcg rng(seed);
    for (std::size_t i = 0; i < total; ++i) base[i] = rng.next();
    for (int i = 0; i < 3; ++i) {
        stackcore::gaussian_blur_5tap(base.data(), tmp.data(), m, m,
                                      static_cast<std::size_t>(m), scratch.data());
        base.swap(tmp);
    }

    // ぼかしで縮んだ振幅を 0.2〜0.8 に伸ばす。暗すぎると自動判定が惑星側に倒れる。
    float lo = base[0], hi = base[0];
    for (std::size_t i = 0; i < total; ++i) {
        if (base[i] < lo) lo = base[i];
        if (base[i] > hi) hi = base[i];
    }
    const float scale = hi > lo ? 0.6f / (hi - lo) : 0.0f;

    FrameBuffer fb(n, n, 1);
    for (int y = 0; y < n; ++y) {
        float* row = fb.row(0, y);
        const std::size_t sy = static_cast<std::size_t>(y + margin + oy);
        for (int x = 0; x < n; ++x) {
            const std::size_t sx = static_cast<std::size_t>(x + margin + ox);
            row[x] = 0.2f + (base[sy * m + sx] - lo) * scale;
        }
    }
    fb.invalidate_luma();
    return fb;
}

FrameBuffer pure_noise(int n, std::uint32_t seed) {
    FrameBuffer fb(n, n, 1);
    Lcg rng(seed);
    for (int y = 0; y < n; ++y) {
        float* row = fb.row(0, y);
        for (int x = 0; x < n; ++x) row[x] = rng.next();
    }
    fb.invalidate_luma();
    return fb;
}

FrameBuffer flat(int n, float value) {
    FrameBuffer fb(n, n, 1);
    for (int y = 0; y < n; ++y) {
        float* row = fb.row(0, y);
        for (int x = 0; x < n; ++x) row[x] = value;
    }
    fb.invalidate_luma();
    return fb;
}

}  // namespace

MT_TEST(align_自動判定は暗背景を惑星モードにする) {
    MT_CHECK(stackcore::detect_align_mode(planet(128, 64, 64, 20, 1)) == AlignMode::Planet);
}

MT_TEST(align_自動判定は全面模様を広視野モードにする) {
    MT_CHECK(stackcore::detect_align_mode(wide_field(128, 0, 0, 1)) == AlignMode::Lunar);
}

MT_TEST(align_惑星モードで既知の変位を復元する) {
    const FrameBuffer ref = planet(128, 64, 64, 20, 1);
    GlobalAligner aligner(ref, GlobalAlignSettings{});
    MT_CHECK(aligner.mode() == AlignMode::Planet);

    const GlobalAlignResult r = aligner.align(planet(128, 73, 59, 20, 1));
    MT_CHECK(r.accepted);
    MT_CHECK_EQ(r.dx, -9);
    MT_CHECK_EQ(r.dy, 5);
}

MT_TEST(align_重心が位相相関だけでは苦しい大変位を吸収する) {
    // 円盤どうしの重なりが乏しくなる程度に離しても、重心の粗補正が効けば復元できる。
    const FrameBuffer ref = planet(128, 64, 64, 12, 1);
    GlobalAligner aligner(ref, GlobalAlignSettings{});
    const GlobalAlignResult r = aligner.align(planet(128, 92, 40, 12, 1));
    MT_CHECK(r.accepted);
    MT_CHECK_EQ(r.dx, -28);
    MT_CHECK_EQ(r.dy, 24);
}

MT_TEST(align_広視野モードで既知の変位を復元する) {
    const FrameBuffer ref = wide_field(128, 0, 0, 2);
    GlobalAligner aligner(ref, GlobalAlignSettings{});
    MT_CHECK(aligner.mode() == AlignMode::Lunar);

    // 基底画像を (+6, -4) だけ先の位置から切り出したフレーム。
    // 見えている模様は参照より左上へ寄るので、合わせ戻す量は (+6, -4)。
    const GlobalAlignResult r = aligner.align(wide_field(128, 6, -4, 2));
    MT_CHECK(r.accepted);
    MT_CHECK_EQ(r.dx, 6);
    MT_CHECK_EQ(r.dy, -4);
}

// --- 追跡失敗の除外 -------------------------------------------------------
// 実データ（PIPPでクロップ済み＝すでに追尾されている）では失敗フレームが
// 出ないため、除外が働くことは合成データでしか確認できない。
// 「実データで全フレーム採用された」ことを除外機構が動いた証拠にしないこと。

MT_TEST(align_純ノイズのフレームは相関不足で除外される) {
    const FrameBuffer ref = planet(128, 64, 64, 20, 1);
    GlobalAligner aligner(ref, GlobalAlignSettings{});
    const GlobalAlignResult r = aligner.align(pure_noise(128, 4242));
    MT_CHECK(!r.accepted);
    MT_CHECK(r.reason == RejectReason::LowCorrelation);
}

MT_TEST(align_一様なフレームは相関不足で除外される) {
    // 曇りやドロップフレームの代用。窓掛けした定数画像はゆるいピークを作るため
    // 純ノイズより高いピーク比が出る。しきい値はこれを跨いで設定してある。
    const FrameBuffer ref = planet(128, 64, 64, 20, 1);
    GlobalAligner aligner(ref, GlobalAlignSettings{});
    const GlobalAlignResult r = aligner.align(flat(128, 0.02f));
    MT_CHECK(!r.accepted);
    MT_CHECK(r.reason == RejectReason::LowCorrelation);
}

MT_TEST(align_視野外へ流れたフレームは変位超過で除外される) {
    const FrameBuffer ref = planet(128, 64, 64, 12, 1);
    GlobalAligner aligner(ref, GlobalAlignSettings{});
    MT_CHECK_EQ(aligner.max_shift(), 32);  // 短辺の1/4

    const GlobalAlignResult r = aligner.align(planet(128, 110, 64, 12, 1));
    MT_CHECK(!r.accepted);
    MT_CHECK(r.reason == RejectReason::ShiftTooLarge);
}

MT_TEST(align_一様なフレームは品質評価でも最下位になる) {
    // 相関による除外と品質による除外は独立した2つの網である。
    stackcore::QualityWorkspace ws;
    const double blank = stackcore::gradient_energy(flat(128, 0.02f), ws);
    const double real = stackcore::gradient_energy(planet(128, 64, 64, 20, 1), ws);
    MT_CHECK(blank < real * 1e-3);
}

MT_TEST(align_モードは手動で上書きできる) {
    GlobalAlignSettings s;
    s.mode = AlignMode::Lunar;
    GlobalAligner aligner(planet(128, 64, 64, 20, 1), s);
    MT_CHECK(aligner.mode() == AlignMode::Lunar);
}
