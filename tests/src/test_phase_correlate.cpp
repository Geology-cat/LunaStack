#include <cmath>
#include <cstdint>
#include <vector>

#include "microtest.hpp"
#include "stackcore/frame_buffer.hpp"
#include "stackcore/phase_correlate.hpp"

using stackcore::CorrelationPeak;
using stackcore::FrameBuffer;
using stackcore::PhaseCorrelator;

namespace {

struct Lcg {
    std::uint32_t s;
    explicit Lcg(std::uint32_t seed) : s(seed) {}
    float next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>((s >> 8) & 0xFFFF) / 65535.0f;
    }
};

// 中央に円盤、少しノイズ。実画像に近い「相関の取りやすい」対象。
FrameBuffer make_scene(int w, int h, double cx, double cy, double radius,
                       std::uint32_t seed) {
    FrameBuffer fb(w, h, 1);
    Lcg rng(seed);
    for (int y = 0; y < h; ++y) {
        float* row = fb.row(0, y);
        for (int x = 0; x < w; ++x) {
            const double dx = x - cx, dy = y - cy;
            const double r = std::sqrt(dx * dx + dy * dy);
            double v = 0.02;
            if (r <= radius) {
                // 縞と斑点で位置が一意に決まる模様にする。
                v = 0.55 + 0.25 * std::sin(dy * 0.9) + 0.12 * std::cos(dx * 1.3);
            }
            v += 0.01 * (rng.next() - 0.5);
            row[x] = static_cast<float>(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
        }
    }
    fb.invalidate_luma();
    return fb;
}

// 一様乱数だけの画像。構造がないので相関は立たない。
FrameBuffer make_noise(int w, int h, std::uint32_t seed) {
    FrameBuffer fb(w, h, 1);
    Lcg rng(seed);
    for (int y = 0; y < h; ++y) {
        float* row = fb.row(0, y);
        for (int x = 0; x < w; ++x) row[x] = rng.next();
    }
    fb.invalidate_luma();
    return fb;
}

CorrelationPeak run(const FrameBuffer& ref, const FrameBuffer& cur) {
    PhaseCorrelator pc(ref.width(), ref.height());
    pc.set_reference(ref.plane(0), ref.width(), ref.height(), ref.stride());
    return pc.correlate(cur.plane(0), cur.width(), cur.height(), cur.stride());
}

}  // namespace

MT_TEST(phase_同一画像の変位はゼロ) {
    const FrameBuffer a = make_scene(64, 64, 32.0, 32.0, 14.0, 3);
    const CorrelationPeak p = run(a, a);
    MT_CHECK_EQ(p.dx, 0);
    MT_CHECK_EQ(p.dy, 0);
}

MT_TEST(phase_既知の変位を符号込みで復元する) {
    // 対象が右下（+5, +3）へ動いたフレームを作る。
    // 返り値は「現在フレームをこれだけ動かすと参照に重なる」量なので
    // (-5, -3) でなければならない。符号を取り違えるとスタックが
    // 変位を倍にして悪化するため、ここを固定しておく。
    const FrameBuffer ref = make_scene(64, 64, 32.0, 32.0, 14.0, 3);
    const FrameBuffer cur = make_scene(64, 64, 37.0, 35.0, 14.0, 3);
    const CorrelationPeak p = run(ref, cur);
    MT_CHECK_EQ(p.dx, -5);
    MT_CHECK_EQ(p.dy, -3);
}

MT_TEST(phase_負の変位も復元する) {
    const FrameBuffer ref = make_scene(64, 64, 32.0, 32.0, 14.0, 3);
    const FrameBuffer cur = make_scene(64, 64, 25.0, 28.0, 14.0, 3);
    const CorrelationPeak p = run(ref, cur);
    MT_CHECK_EQ(p.dx, 7);
    MT_CHECK_EQ(p.dy, 4);
}

MT_TEST(phase_非正方の画像でも軸を取り違えない) {
    // 幅と高さが違う場合に x と y を入れ替えて実装していないかを見る。
    const FrameBuffer ref = make_scene(96, 48, 48.0, 24.0, 12.0, 5);
    const FrameBuffer cur = make_scene(96, 48, 54.0, 26.0, 12.0, 5);
    const CorrelationPeak p = run(ref, cur);
    MT_CHECK_EQ(p.dx, -6);
    MT_CHECK_EQ(p.dy, -2);
}

MT_TEST(phase_明るさが変わっても変位は変わらない) {
    // 位相相関は振幅を捨てるので、露出差に強いはず。
    const FrameBuffer ref = make_scene(64, 64, 32.0, 32.0, 14.0, 3);
    FrameBuffer cur = make_scene(64, 64, 36.0, 32.0, 14.0, 3);
    for (int y = 0; y < cur.height(); ++y) {
        float* row = cur.row(0, y);
        for (int x = 0; x < cur.width(); ++x) row[x] *= 0.6f;
    }
    cur.invalidate_luma();
    const CorrelationPeak p = run(ref, cur);
    MT_CHECK_EQ(p.dx, -4);
    MT_CHECK_EQ(p.dy, 0);
}

MT_TEST(phase_相関が立つ場合と立たない場合でピーク比に差が出る) {
    // 追跡失敗の判定を peak_sidelobe_ratio に頼るので、両者が明確に分かれることを確かめる。
    const FrameBuffer ref = make_scene(64, 64, 32.0, 32.0, 14.0, 3);
    const FrameBuffer shifted = make_scene(64, 64, 36.0, 30.0, 14.0, 3);
    const FrameBuffer noise = make_noise(64, 64, 99);

    const double good = run(ref, shifted).peak_sidelobe_ratio;
    const double bad = run(ref, noise).peak_sidelobe_ratio;

    if (!(good > bad * 2.0)) {
        microtest::fail("相関が立つ場合のピーク比が十分大きくない: good=" +
                        microtest::mt_str(good) + " bad=" + microtest::mt_str(bad));
    }
}

MT_TEST(phase_パディングは正方の2の冪になる) {
    PhaseCorrelator pc(96, 48);
    MT_CHECK_EQ(pc.padded_size(), 128);
    MT_CHECK_EQ(pc.max_shift(), 64);
}
