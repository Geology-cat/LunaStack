#include <cmath>
#include <cstdint>

#include "microtest.hpp"
#include "stackcore/frame_buffer.hpp"
#include "stackcore/simple_stacker.hpp"

using stackcore::FrameBuffer;
using stackcore::SimpleStacker;
using stackcore::StackStats;

namespace {

struct Lcg {
    std::uint32_t s;
    explicit Lcg(std::uint32_t seed) : s(seed) {}
    float next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>((s >> 8) & 0xFFFF) / 65535.0f;
    }
};

FrameBuffer constant(int w, int h, int c, float value) {
    FrameBuffer fb(w, h, c);
    for (int ch = 0; ch < c; ++ch) {
        for (int y = 0; y < h; ++y) {
            float* row = fb.row(ch, y);
            for (int x = 0; x < w; ++x) row[x] = value;
        }
    }
    fb.invalidate_luma();
    return fb;
}

// なめらかな信号。スタックのS/N測定の真値に使う。
float clean_value(int x, int y) {
    return static_cast<float>(0.5 + 0.25 * std::sin(x * 0.2) * std::cos(y * 0.17));
}

FrameBuffer clean(int n) {
    FrameBuffer fb(n, n, 1);
    for (int y = 0; y < n; ++y) {
        float* row = fb.row(0, y);
        for (int x = 0; x < n; ++x) row[x] = clean_value(x, y);
    }
    fb.invalidate_luma();
    return fb;
}

FrameBuffer noisy(int n, std::uint32_t seed, double amplitude) {
    FrameBuffer fb(n, n, 1);
    Lcg rng(seed);
    for (int y = 0; y < n; ++y) {
        float* row = fb.row(0, y);
        for (int x = 0; x < n; ++x) {
            const double v = clean_value(x, y) + amplitude * (rng.next() - 0.5);
            row[x] = static_cast<float>(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
        }
    }
    fb.invalidate_luma();
    return fb;
}

double rms_error(const FrameBuffer& a, const FrameBuffer& reference, int margin) {
    double sum = 0.0;
    int count = 0;
    for (int y = margin; y < a.height() - margin; ++y) {
        const float* ra = a.row(0, y);
        const float* rb = reference.row(0, y);
        for (int x = margin; x < a.width() - margin; ++x) {
            const double d = static_cast<double>(ra[x]) - rb[x];
            sum += d * d;
            ++count;
        }
    }
    return std::sqrt(sum / count);
}

}  // namespace

MT_TEST(stack_同じ値のフレームを重ねても値が変わらない) {
    SimpleStacker st(16, 12, 3);
    for (int i = 0; i < 5; ++i) st.add(constant(16, 12, 3, 0.4f), 0, 0, 1.0);

    FrameBuffer out;
    StackStats stats;
    st.finish(out, stats);

    MT_CHECK_EQ(stats.frames, 5);
    MT_CHECK_EQ(static_cast<int>(stats.min_coverage), 5);
    MT_CHECK_EQ(static_cast<int>(stats.max_coverage), 5);
    for (int c = 0; c < 3; ++c) {
        for (int y = 0; y < 12; ++y) {
            const float* row = out.row(c, y);
            for (int x = 0; x < 16; ++x) MT_CHECK_NEAR(row[x], 0.4f, 1e-6);
        }
    }
}

MT_TEST(stack_変位を与えると寄与のない画素ができる) {
    SimpleStacker st(8, 8, 1);
    st.add(constant(8, 8, 1, 1.0f), 2, 0, 1.0);  // 右に2ずらす

    FrameBuffer out;
    StackStats stats;
    st.finish(out, stats);

    MT_CHECK_EQ(static_cast<int>(stats.min_coverage), 0);
    MT_CHECK_EQ(static_cast<int>(stats.max_coverage), 1);
    for (int y = 0; y < 8; ++y) {
        const float* row = out.row(0, y);
        MT_CHECK_NEAR(row[0], 0.0f, 1e-6);  // 左端2列は誰も寄与しない
        MT_CHECK_NEAR(row[1], 0.0f, 1e-6);
        MT_CHECK_NEAR(row[2], 1.0f, 1e-6);
        MT_CHECK_NEAR(row[7], 1.0f, 1e-6);
    }
}

MT_TEST(stack_変位したフレームが正しい位置に重なる) {
    // 1画素だけ明るい画像を (3, 2) 動かして加算すると、その画素が移動するはず。
    FrameBuffer dot(16, 16, 1);
    dot.row(0, 5)[4] = 1.0f;
    dot.invalidate_luma();

    SimpleStacker st(16, 16, 1);
    st.add(dot, 3, 2, 1.0);

    FrameBuffer out;
    StackStats stats;
    st.finish(out, stats);
    MT_CHECK_NEAR(out.row(0, 7)[7], 1.0f, 1e-6);
    MT_CHECK_NEAR(out.row(0, 5)[4], 0.0f, 1e-6);
}

MT_TEST(stack_輝度正規化の係数が掛かる) {
    SimpleStacker st(8, 8, 1);
    st.add(constant(8, 8, 1, 0.25f), 0, 0, 2.0);

    FrameBuffer out;
    StackStats stats;
    st.finish(out, stats);
    MT_CHECK_NEAR(out.row(0, 0)[0], 0.5f, 1e-6);
}

MT_TEST(stack_1を超えた画素は切り詰められ件数が記録される) {
    SimpleStacker st(4, 4, 1);
    st.add(constant(4, 4, 1, 0.8f), 0, 0, 2.0);  // 1.6 になる

    FrameBuffer out;
    StackStats stats;
    st.finish(out, stats);
    MT_CHECK_EQ(static_cast<int>(stats.clipped), 16);
    MT_CHECK_NEAR(out.row(0, 0)[0], 1.0f, 1e-6);
    if (!(stats.max_value > 1.5)) {
        microtest::fail("切り詰め前の最大値が記録されていない: " +
                        microtest::mt_str(stats.max_value));
    }
}

MT_TEST(stack_加算するほどノイズが減る) {
    // M1の受け入れ条件「1フレームより明確にS/Nが向上している」の合成データ版。
    const int n = 64;
    const double amp = 0.20;
    const FrameBuffer truth = clean(n);

    const double single = rms_error(noisy(n, 1, amp), truth, 2);

    SimpleStacker st(n, n, 1);
    for (int i = 0; i < 64; ++i) st.add(noisy(n, 1000u + i, amp), 0, 0, 1.0);
    FrameBuffer stacked;
    StackStats stats;
    st.finish(stacked, stats);
    const double many = rms_error(stacked, truth, 2);

    // 独立ノイズなら 64枚で 1/8 になるはず。実装の取りこぼしを見たいので
    // 理論値の半分（1/4）までは達していることを要求する。
    if (!(many < single / 4.0)) {
        microtest::fail("スタックでノイズが十分減っていない: 1枚=" +
                        microtest::mt_str(single) + " 64枚=" + microtest::mt_str(many));
    }
}

MT_TEST(stack_加算順序を変えない限り結果はビット単位で同じ) {
    const int n = 32;
    FrameBuffer a, b;
    StackStats sa, sb;

    SimpleStacker s1(n, n, 1);
    for (int i = 0; i < 16; ++i) s1.add(noisy(n, 500u + i, 0.1), i % 3 - 1, i % 5 - 2, 1.0);
    s1.finish(a, sa);

    SimpleStacker s2(n, n, 1);
    for (int i = 0; i < 16; ++i) s2.add(noisy(n, 500u + i, 0.1), i % 3 - 1, i % 5 - 2, 1.0);
    s2.finish(b, sb);

    for (int y = 0; y < n; ++y) {
        const float* ra = a.row(0, y);
        const float* rb = b.row(0, y);
        for (int x = 0; x < n; ++x) {
            if (ra[x] != rb[x]) {
                microtest::fail("同じ順序の加算で結果が一致しない (" +
                                microtest::mt_str(x) + "," + microtest::mt_str(y) + ")");
            }
        }
    }
}
