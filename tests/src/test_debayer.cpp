#include "microtest.hpp"
#include "stackcore/debayer.hpp"
#include "stackcore/frame_buffer.hpp"
#include "stackcore/ser_decoder.hpp"

using stackcore::FrameBuffer;
using stackcore::SerColorId;

namespace {

// RGGB 配列における (x, y) の色（0=R, 1=G, 2=B）。テスト側で独立に定義し、
// 実装と同じ表を使い回さないことで取り違えを検出できるようにする。
int rggb_color(int x, int y) {
    if ((y & 1) == 0) return (x & 1) == 0 ? 0 : 1;
    return (x & 1) == 0 ? 1 : 2;
}

}  // namespace

MT_TEST(debayer_一様な入力は全チャンネル同じ値になる) {
    const int w = 16, h = 12;
    FrameBuffer cfa(w, h, 1);
    for (int y = 0; y < h; ++y) {
        float* row = cfa.row(0, y);
        for (int x = 0; x < w; ++x) row[x] = 0.375f;
    }

    FrameBuffer rgb;
    stackcore::debayer_bilinear(cfa, SerColorId::BayerRGGB, rgb);

    MT_CHECK_EQ(rgb.width(), w);
    MT_CHECK_EQ(rgb.height(), h);
    MT_CHECK_EQ(rgb.channels(), 3);

    for (int c = 0; c < 3; ++c) {
        for (int y = 0; y < h; ++y) {
            const float* row = rgb.row(c, y);
            for (int x = 0; x < w; ++x) MT_CHECK_NEAR(row[x], 0.375, 1e-6);
        }
    }
}

MT_TEST(debayer_赤のみの被写体は内部でRが一定になりGとBがゼロになる) {
    // R位置だけ1.0、他は0.0。正しく補間できていれば内部画素の
    // Rチャンネルは全域1.0、G/Bチャンネルは0.0になる。
    const int w = 16, h = 12;
    FrameBuffer cfa(w, h, 1);
    for (int y = 0; y < h; ++y) {
        float* row = cfa.row(0, y);
        for (int x = 0; x < w; ++x) row[x] = rggb_color(x, y) == 0 ? 1.0f : 0.0f;
    }

    FrameBuffer rgb;
    stackcore::debayer_bilinear(cfa, SerColorId::BayerRGGB, rgb);

    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            MT_CHECK_NEAR(rgb.row(0, y)[x], 1.0, 1e-6);
            MT_CHECK_NEAR(rgb.row(1, y)[x], 0.0, 1e-6);
            MT_CHECK_NEAR(rgb.row(2, y)[x], 0.0, 1e-6);
        }
    }
}

MT_TEST(debayer_パターン違いで色の割り当てが変わる) {
    // 同じCFAデータでも、BGGRとして解釈すればR位置とB位置が入れ替わる。
    const int w = 8, h = 8;
    FrameBuffer cfa(w, h, 1);
    for (int y = 0; y < h; ++y) {
        float* row = cfa.row(0, y);
        for (int x = 0; x < w; ++x) row[x] = rggb_color(x, y) == 0 ? 1.0f : 0.0f;
    }

    FrameBuffer as_rggb, as_bggr;
    stackcore::debayer_bilinear(cfa, SerColorId::BayerRGGB, as_rggb);
    stackcore::debayer_bilinear(cfa, SerColorId::BayerBGGR, as_bggr);

    // RGGBでRだった成分は、BGGRではBに現れる。
    MT_CHECK_NEAR(as_rggb.row(0, 4)[4], 1.0, 1e-6);
    MT_CHECK_NEAR(as_bggr.row(2, 4)[4], 1.0, 1e-6);
    MT_CHECK_NEAR(as_bggr.row(0, 4)[4], 0.0, 1e-6);
}

MT_TEST(debayer_未対応パターンは例外になる) {
    FrameBuffer cfa(8, 8, 1);
    FrameBuffer rgb;
    MT_CHECK_THROWS(stackcore::debayer_bilinear(cfa, SerColorId::BayerCYYM, rgb));
    MT_CHECK_THROWS(stackcore::debayer_bilinear(cfa, SerColorId::Mono, rgb));
}

MT_TEST(debayer_入力が1chでなければ例外になる) {
    FrameBuffer rgb_input(8, 8, 3);
    FrameBuffer out;
    MT_CHECK_THROWS(stackcore::debayer_bilinear(rgb_input, SerColorId::BayerRGGB, out));
}
