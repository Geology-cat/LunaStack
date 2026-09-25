// 静止画の読み込み（TIFF / PNG / FITS）と DEFLATE 展開のテスト。
//
// 試験画像（tests/data）は PIL・tifffile・libtiff の tiffcp で作ったもので、
// LunaStack 自身の書き出しではない。「他のソフトが書いたファイルを読める」ことの確認である。

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "image_fixtures.hpp"
#include "microtest.hpp"
#include "stackcore/fits_writer.hpp"
#include "stackcore/image_reader.hpp"
#include "stackcore/inflate.hpp"
#include "stackcore/png_writer.hpp"
#include "stackcore/tiff_writer.hpp"
#include "stackcore/video_source.hpp"

using stackcore::FrameBuffer;
using stackcore::ImageFileInfo;

namespace {

std::string data_path(const char* name) { return std::string(LUNASTACK_TEST_DATA_DIR) + "/" + name; }
std::string temp_path(const char* name) { return std::string("/tmp/lunastack_test_") + name; }

// 8bitの期待値と画素を比べる（許容差は量子化の半分）。
void expect_pattern8(const FrameBuffer& f, int channels) {
    MT_CHECK_EQ(f.width(), fixtures::kWidth);
    MT_CHECK_EQ(f.height(), fixtures::kHeight);
    MT_CHECK_EQ(f.channels(), channels);
    for (int c = 0; c < channels; ++c) {
        for (int y = 0; y < fixtures::kHeight; ++y) {
            for (int x = 0; x < fixtures::kWidth; ++x) {
                MT_CHECK_NEAR(f.row(c, y)[x], fixtures::pat8(x, y, c) / 255.0, 1e-6);
            }
        }
    }
}

void expect_pattern16(const FrameBuffer& f, int channels) {
    MT_CHECK_EQ(f.width(), fixtures::kWidth);
    MT_CHECK_EQ(f.height(), fixtures::kHeight);
    MT_CHECK_EQ(f.channels(), channels);
    for (int c = 0; c < channels; ++c) {
        for (int y = 0; y < fixtures::kHeight; ++y) {
            for (int x = 0; x < fixtures::kWidth; ++x) {
                MT_CHECK_NEAR(f.row(c, y)[x], fixtures::pat16(x, y, c) / 65535.0, 1e-6);
            }
        }
    }
}

}  // namespace

MT_TEST(inflate_無圧縮と固定と動的ハフマンのzlibを展開できる) {
    // Python の zlib で作ったストリーム（無圧縮 / Z_FIXED / 既定の動的ハフマン）。
    const std::vector<std::uint8_t> stored = {
        0x78, 0x01, 0x01, 0x0a, 0x00, 0xf5, 0xff, 0x61, 0x62, 0x63, 0x61, 0x62, 0x63, 0x61,
        0x62, 0x63, 0x0a, 0x14, 0xba, 0x03, 0x7d};
    const std::vector<std::uint8_t> fixed = {
        0x78, 0x01, 0xf3, 0x29, 0xcd, 0x4b, 0x0c, 0x2e, 0x49, 0x4c, 0xce, 0x56, 0x30, 0x50,
        0xc8, 0xcd, 0xcf, 0xcf, 0x53, 0x28, 0xc8, 0x49, 0xcc, 0x4b, 0x2d, 0x51, 0x28, 0x06,
        0x8b, 0x95, 0x27, 0x96, 0xa5, 0xe6, 0x00, 0x79, 0x29, 0x45, 0x99, 0x55, 0x55, 0x39,
        0xa9, 0x5c, 0x3e, 0x70, 0xd5, 0x86, 0x24, 0xa9, 0x36, 0x21, 0x49, 0xb5, 0x25, 0x69,
        0x2e, 0x31, 0x23, 0x49, 0xb9, 0x91, 0x29, 0x49, 0xca, 0x8d, 0x49, 0x33, 0xdd, 0x84,
        0x34, 0xb7, 0x9b, 0x91, 0x16, 0x30, 0x16, 0xa4, 0x85, 0xba, 0x31, 0x69, 0x01, 0x43,
        0x9a, 0x5b, 0x4c, 0xcc, 0x49, 0x52, 0x6e, 0x6e, 0x44, 0x9a, 0x63, 0x48, 0xf3, 0x28,
        0x69, 0xe1, 0x62, 0x46, 0x9a, 0xe9, 0x96, 0x24, 0xa6, 0x18, 0xd2, 0x82, 0xdd, 0x9c,
        0xc4, 0x7c, 0x47, 0x9a, 0xdb, 0x4d, 0x49, 0x73, 0x8c, 0x25, 0x89, 0xa9, 0x9d, 0xc4,
        0x7c, 0x4d, 0x62, 0xa1, 0x41, 0xa2, 0xdb, 0x49, 0x73, 0x8c, 0x29, 0x69, 0xe1, 0x6e,
        0x41, 0x5a, 0x0a, 0x23, 0x2d, 0xc9, 0x18, 0x91, 0x96, 0x97, 0x2c, 0x48, 0x73, 0x8c,
        0x29, 0x69, 0x01, 0x63, 0x44, 0x5a, 0x0a, 0xb3, 0x20, 0xb1, 0xc4, 0x23, 0xb1, 0x08,
        0x23, 0x2d, 0x20, 0x0d, 0x49, 0x33, 0xdd, 0x82, 0xb4, 0xe4, 0x6e, 0x46, 0x62, 0xee,
        0x20, 0x2d, 0x9a, 0x8c, 0x49, 0x0b, 0x77, 0x43, 0x12, 0x53, 0x24, 0x69, 0x79, 0x89,
        0xb4, 0xac, 0x67, 0x41, 0x5a, 0x2c, 0x99, 0x93, 0x96, 0x64, 0xcc, 0x49, 0x34, 0xdd,
        0x98, 0xa6, 0xca, 0x69, 0xea, 0x55, 0x12, 0x03, 0x92, 0xc4, 0x68, 0x32, 0xa3, 0x65,
        0x02, 0x23, 0x31, 0xf9, 0x92, 0x98, 0x39, 0x48, 0xcc, 0x7a, 0x44, 0x65, 0x6c, 0x00,
        0x86, 0x99, 0xfc, 0x5f};
    const std::vector<std::uint8_t> dynamic = {
        0x78, 0xda, 0xb5, 0xd4, 0xbb, 0x0d, 0xc2, 0x40, 0x10, 0x84, 0xe1, 0x9c, 0x2a, 0x5c,
        0x02, 0x7e, 0xdd, 0x9d, 0x7b, 0x70, 0x46, 0x05, 0x27, 0x70, 0x80, 0x30, 0x36, 0xe2,
        0x29, 0xb9, 0x7a, 0x10, 0x01, 0x31, 0x5f, 0xe0, 0xd0, 0xd6, 0x68, 0x35, 0x37, 0xfb,
        0xcf, 0xf6, 0x8f, 0x29, 0xef, 0xee, 0x79, 0x7f, 0x2a, 0xb6, 0xc5, 0x79, 0x9e, 0xa7,
        0xe2, 0x32, 0xe6, 0x69, 0xb8, 0x17, 0xb7, 0xef, 0xbf, 0x57, 0x7e, 0x0e, 0xe3, 0xe7,
        0xeb, 0x70, 0x3d, 0x2e, 0xcb, 0x38, 0x6c, 0xfa, 0x9f, 0xba, 0x24, 0x75, 0x43, 0xea,
        0xce, 0x9c, 0x04, 0x92, 0x57, 0x2d, 0xc9, 0x6b, 0x9b, 0xde, 0x98, 0xf7, 0x60, 0xc1,
        0x24, 0x4b, 0xbd, 0xb6, 0x60, 0xcc, 0x4b, 0x13, 0x49, 0x1e, 0x2b, 0x33, 0x63, 0x0f,
        0xb5, 0x5c, 0x82, 0x4d, 0xef, 0x90, 0x18, 0x8b, 0x3d, 0x62, 0xef, 0xcc, 0x7b, 0x6b,
        0x66, 0x3a, 0xa4, 0x1d, 0x7b, 0x8d, 0x47, 0x03, 0xbd, 0x9b, 0x99, 0xd6, 0x72, 0x4f,
        0x46, 0x98, 0x21, 0x53, 0x59, 0x97, 0x92, 0x99, 0x69, 0x2d, 0x98, 0xca, 0x08, 0x4b,
        0x78, 0xf1, 0xf0, 0x84, 0x59, 0x90, 0xa5, 0x4d, 0x4f, 0x86, 0x7b, 0xc0, 0x76, 0xd8,
        0x9a, 0x6a, 0xcb, 0xbd, 0x44, 0x22, 0xad, 0x4b, 0x56, 0xbd, 0x64, 0x5b, 0x8a, 0x86,
        0x4c, 0xc4, 0xe9, 0xf5, 0xaa, 0xf2, 0x55, 0x9f, 0x8a, 0x41, 0xe2, 0x9a, 0xc2, 0x9a,
        0x80, 0x21, 0xbe, 0x58, 0x0e, 0xac, 0xde, 0x5f, 0xc5, 0x7e, 0x03, 0x86, 0x99, 0xfc, 0x5f};
    std::string text;
    for (int i = 0; i < 60; ++i) {
        text += "LunaStack " + std::to_string(i * i % 97) + " moon planet stack wavelet drizzle\n";
    }
    std::vector<std::uint8_t> out = stackcore::inflate_zlib(stored.data(), stored.size());
    MT_CHECK_EQ(std::string(out.begin(), out.end()), std::string("abcabcabc\n"));
    out = stackcore::inflate_zlib(fixed.data(), fixed.size());
    MT_CHECK(std::string(out.begin(), out.end()) == text);
    out = stackcore::inflate_zlib(dynamic.data(), dynamic.size());
    MT_CHECK(std::string(out.begin(), out.end()) == text);

    // Adler-32 の破損を検出する。
    std::vector<std::uint8_t> broken = dynamic;
    broken.back() ^= 0x01;
    MT_CHECK_THROWS(stackcore::inflate_zlib(broken.data(), broken.size()));
    // 展開後の上限を超えたら打ち切る。
    MT_CHECK_THROWS(stackcore::inflate_zlib(dynamic.data(), dynamic.size(), 100));
}

MT_TEST(image_他のソフトが書いた圧縮TIFFを読める) {
    FrameBuffer f;
    ImageFileInfo info;
    stackcore::read_image_file(data_path("gray8_lzw.tif"), f, info);
    expect_pattern8(f, 1);
    MT_CHECK_EQ(info.bit_depth, 8);

    stackcore::read_image_file(data_path("rgb8_deflate.tif"), f, info);
    expect_pattern8(f, 3);
    stackcore::read_image_file(data_path("rgb8_packbits.tif"), f, info);
    expect_pattern8(f, 3);

    // LZW + 水平差分予測 + ビッグエンディアン
    stackcore::read_image_file(data_path("gray16_pred_lzw_be.tif"), f, info);
    expect_pattern16(f, 1);
    MT_CHECK_EQ(info.bit_depth, 16);

    // プレーナ配置 + タイル（画像より大きい16×16タイル）+ Deflate + 予測
    stackcore::read_image_file(data_path("rgb16_planar_tiled_deflate.tif"), f, info);
    expect_pattern16(f, 3);

    // 32bit浮動小数点 + 浮動小数点予測子 + 複数ストリップ
    stackcore::read_image_file(data_path("float32_pred3_strips.tif"), f, info);
    expect_pattern16(f, 1);
    MT_CHECK_EQ(info.bit_depth, 32);
}

MT_TEST(image_PNGのグレー・RGB・アルファ・パレットを読める) {
    FrameBuffer f;
    ImageFileInfo info;
    stackcore::read_image_file(data_path("rgb8.png"), f, info);
    expect_pattern8(f, 3);
    stackcore::read_image_file(data_path("gray16.png"), f, info);
    expect_pattern16(f, 1);
    stackcore::read_image_file(data_path("rgba8.png"), f, info);
    expect_pattern8(f, 3);  // アルファは捨てる

    stackcore::read_image_file(data_path("palette.png"), f, info);
    MT_CHECK_EQ(f.channels(), 3);
    for (int y = 0; y < fixtures::kHeight; ++y) {
        for (int x = 0; x < fixtures::kWidth; ++x) {
            for (int c = 0; c < 3; ++c) {
                const int expected =
                    fixtures::kPaletteRgb[(y * fixtures::kWidth + x) * 3 + c];
                MT_CHECK_NEAR(f.row(c, y)[x], expected / 255.0, 1e-6);
            }
        }
    }
}

MT_TEST(image_FITSのBZEROとBAYERPATとROWORDERを解釈する) {
    FrameBuffer f;
    ImageFileInfo info;
    stackcore::read_image_file(data_path("bayer16_bottomup.fits"), f, info);
    // ファイルは下端の行から並んでいるが、ROWORDER=BOTTOM-UP なので上下が戻る。
    expect_pattern16(f, 1);
    MT_CHECK(info.color == stackcore::SerColorId::BayerRGGB);
}

MT_TEST(image_自前の書き出しと往復できる) {
    FrameBuffer src(17, 11, 3);
    for (int c = 0; c < 3; ++c) {
        for (int y = 0; y < 11; ++y) {
            for (int x = 0; x < 17; ++x) src.row(c, y)[x] = ((x * 7 + y * 13 + c * 29) % 97) / 96.0f;
        }
    }
    const std::string png = temp_path("roundtrip.png");
    const std::string tif = temp_path("roundtrip.tif");
    const std::string fits = temp_path("roundtrip.fits");
    stackcore::write_png16(png, src);
    stackcore::write_tiff(tif, src, stackcore::TiffFormat::Float32);
    stackcore::write_fits_float32(fits, src);
    for (const std::string& path : {png, tif, fits}) {
        FrameBuffer back;
        ImageFileInfo info;
        stackcore::read_image_file(path, back, info);
        MT_CHECK_EQ(back.width(), 17);
        MT_CHECK_EQ(back.channels(), 3);
        const double tol = path == png ? 1.0 / 65535.0 : 1e-7;
        for (int c = 0; c < 3; ++c) {
            for (int y = 0; y < 11; ++y) {
                for (int x = 0; x < 17; ++x) MT_CHECK_NEAR(back.row(c, y)[x], src.row(c, y)[x], tol);
            }
        }
    }
    std::remove(png.c_str());
    std::remove(tif.c_str());
    std::remove(fits.c_str());
}

MT_TEST(image_自然順で並べる) {
    MT_CHECK(stackcore::natural_less("img2.png", "img10.png"));
    MT_CHECK(!stackcore::natural_less("img10.png", "img2.png"));
    MT_CHECK(stackcore::natural_less("a_001.tif", "a_002.tif"));
    MT_CHECK(stackcore::natural_less("frame9", "frame09a"));
    MT_CHECK(!stackcore::natural_less("x", "x"));
}

MT_TEST(image_壊れたファイルと未対応形式を拒否する) {
    FrameBuffer f;
    ImageFileInfo info;
    MT_CHECK_THROWS(stackcore::read_image_file(data_path("does-not-exist.png"), f, info));
    const std::string bad = temp_path("broken.png");
    std::FILE* fp = std::fopen(bad.c_str(), "wb");
    const char junk[] = "\x89PNG\r\n\x1a\nnot really a png";
    std::fwrite(junk, 1, sizeof(junk), fp);
    std::fclose(fp);
    MT_CHECK_THROWS(stackcore::read_image_file(bad, f, info));
    std::remove(bad.c_str());
    MT_CHECK(!stackcore::is_supported_image_path("movie.ser"));
    MT_CHECK(stackcore::is_supported_image_path("IMG_0001.TIF"));
}
