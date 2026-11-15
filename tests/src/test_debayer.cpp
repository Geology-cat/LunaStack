#include <algorithm>
#include <cstring>

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

namespace {

// 4パターンぶんの色の表（テスト側で独立に持つ）。
int pattern_color(SerColorId pattern, int x, int y) {
    const int px = x & 1, py = y & 1;
    const int rggb[2][2] = {{0, 1}, {1, 2}};
    const int grbg[2][2] = {{1, 0}, {2, 1}};
    const int gbrg[2][2] = {{1, 2}, {0, 1}};
    const int bggr[2][2] = {{2, 1}, {1, 0}};
    switch (pattern) {
        case SerColorId::BayerRGGB: return rggb[py][px];
        case SerColorId::BayerGRBG: return grbg[py][px];
        case SerColorId::BayerGBRG: return gbrg[py][px];
        default: return bggr[py][px];
    }
}

// 定義どおりの bilinear（3x3近傍の同色サンプルを行→列の順に足して平均する）。
float reference_bilinear(const FrameBuffer& cfa, SerColorId pattern, int c, int x, int y) {
    if (pattern_color(pattern, x, y) == c) return cfa.row(0, y)[x];
    float sum = 0.0f;
    int count = 0;
    for (int ny = std::max(0, y - 1); ny <= std::min(cfa.height() - 1, y + 1); ++ny) {
        for (int nx = std::max(0, x - 1); nx <= std::min(cfa.width() - 1, x + 1); ++nx) {
            if (nx == x && ny == y) continue;
            if (pattern_color(pattern, nx, ny) != c) continue;
            sum += cfa.row(0, ny)[nx];
            ++count;
        }
    }
    return count > 0 ? sum / static_cast<float>(count) : cfa.row(0, y)[x];
}

}  // namespace

MT_TEST(debayer_内側の高速処理は定義どおりの補間とビット単位で一致する) {
    const SerColorId patterns[] = {SerColorId::BayerRGGB, SerColorId::BayerGRBG,
                                   SerColorId::BayerGBRG, SerColorId::BayerBGGR};
    const int sizes[][2] = {{37, 22}, {22, 37}, {8, 8}, {3, 3}, {2, 5}, {5, 2}, {1, 1}};
    for (const SerColorId pattern : patterns) {
        for (const auto& size : sizes) {
            const int w = size[0], h = size[1];
            FrameBuffer cfa(w, h, 1);
            unsigned seed = 20261115u + static_cast<unsigned>(w * 131 + h);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    seed = seed * 1664525u + 1013904223u;
                    cfa.row(0, y)[x] = static_cast<float>((seed >> 8) & 0xFFFF) / 65535.0f;
                }
            }
            FrameBuffer rgb;
            stackcore::debayer_bilinear(cfa, pattern, rgb);
            int mismatches = 0;
            for (int c = 0; c < 3; ++c) {
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        const float expected = reference_bilinear(cfa, pattern, c, x, y);
                        if (std::memcmp(&expected, &rgb.row(c, y)[x], sizeof(float)) != 0) {
                            ++mismatches;
                        }
                    }
                }
            }
            MT_CHECK_EQ(mismatches, 0);
        }
    }
}
