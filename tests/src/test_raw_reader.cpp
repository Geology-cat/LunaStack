// カメラのRAWの読み込み（同梱の LibRaw 経由）のテスト。
//
// 試験画像（tests/data/raw_*.dng）は tests/data/make_raw_fixtures.py で作った小さなファイルで、
// 画素値はそこに書いた式 value(x, y) で決まっている。LibRaw が展開した値を、どう切り抜き・
// 黒を引き・0..1 にして渡すか（並びの計算を含む）を確かめる。
// 実機の各社RAW（大きいのでリポジトリには入れない）での確認は開発記録 §8・§9・§10。

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "microtest.hpp"
#include "stackcore/image_reader.hpp"
#include "stackcore/metadata.hpp"
#include "stackcore/raw_reader.hpp"
#include "stackcore/video_source.hpp"

using stackcore::FrameBuffer;
using stackcore::ImageFileInfo;
using stackcore::SerColorId;

namespace {

std::string data_path(const char* name) { return std::string(LUNASTACK_TEST_DATA_DIR) + "/" + name; }

// make_raw_fixtures.py の value(x, y) と同じ式。
int value(int x, int y) { return 200 + ((x * 37 + y * 101 + (x * y) % 7) % 3700); }

// DNG の試験画像の共通の配置。
constexpr int kAreaTop = 4, kAreaLeft = 2;
constexpr int kCropX = kAreaLeft + 2, kCropY = kAreaTop + 2;  // ActiveArea + DefaultCropOrigin
constexpr int kCropW = 33, kCropH = 22;
const double kBlack[4] = {100, 110, 120, 130};  // ActiveArea の左上からの 2×2
constexpr double kWhite = 4000;

double dng_black(int rx, int ry) {
    const int ax = rx - kAreaLeft, ay = ry - kAreaTop;
    return kBlack[(ay % 2) * 2 + (ax % 2)];
}

void expect_cfa_dng(const char* name, SerColorId expected_color, bool linearized) {
    FrameBuffer f;
    ImageFileInfo info;
    stackcore::read_image_file(data_path(name), f, info);
    MT_CHECK_EQ(f.width(), kCropW);
    MT_CHECK_EQ(f.height(), kCropH);
    MT_CHECK_EQ(f.channels(), 1);
    MT_CHECK_EQ(info.width, kCropW);
    MT_CHECK_EQ(info.height, kCropH);
    MT_CHECK(info.color == expected_color);
    MT_CHECK(info.format == "DNG");
    MT_CHECK(info.camera == "LunaStack Test CFA");
    for (int y = 0; y < kCropH; ++y) {
        for (int x = 0; x < kCropW; ++x) {
            const int rx = kCropX + x, ry = kCropY + y;
            double v = value(rx, ry), b = dng_black(rx, ry), w = kWhite;
            if (linearized) {
                // 線形化で2倍、BlackLevel も2倍、行列の差分つき。
                v *= 2;
                b = 2 * b + ((rx - kAreaLeft) % 3) - ((ry - kAreaTop) % 2);
                w *= 2;
            }
            // LibRaw は位相ごとの黒を引いたあと、共通の黒（いちばん小さい黒）を引いた白で割る。
            const double common = linearized ? 2 * kBlack[0] - 1 : kBlack[0];
            // 行列の差分は、LibRaw が平均を黒に足す形で近似する（1画素ごとには引かない）ので、
            // その幅（差分の最大 ±2 程度）だけ許す。
            MT_CHECK_NEAR(f.row(0, y)[x], std::min(1.0, std::max(0.0, (v - b) / (w - common))),
                          linearized ? 4e-4 : 2e-4);
        }
    }
}

}  // namespace

MT_TEST(raw_拡張子でRAWを判定し静止画として扱う) {
    MT_CHECK(stackcore::is_raw_image_path("/a/b/IMG_0001.CR2"));
    MT_CHECK(stackcore::is_raw_image_path("/a/静止画cr2/20250713_0070345 .CR2"));
    MT_CHECK(stackcore::is_raw_image_path("x.dng"));
    MT_CHECK(!stackcore::is_raw_image_path("/a/静止画cr2/readme"));  // フォルダ名に cr2 があっても違う
    MT_CHECK(stackcore::is_raw_image_path("x.CR3"));
    MT_CHECK(stackcore::is_raw_image_path("x.nef") && stackcore::is_raw_image_path("x.ARW") && stackcore::is_raw_image_path("x.raf"));
    MT_CHECK(!stackcore::is_raw_image_path("x.xyz"));
    MT_CHECK(stackcore::is_supported_image_path("IMG_1.CR2"));
    MT_CHECK(stackcore::is_supported_image_path("IMG_1.DNG"));
}

MT_TEST(raw_DNG_16bit無圧縮のCFAをActiveAreaとDefaultCropで切り抜く) {
    // CFAPattern は ActiveArea の左上 (2,4) から数える。切り抜きの左上 (4,6) は (2,2) ずれで
    // 偶数なので GRBG のまま。
    expect_cfa_dng("raw_cfa16_grbg.dng", SerColorId::BayerGRBG, false);
}

MT_TEST(raw_DNG_ロスレスJPEGのタイルとSubIFDを読む) {
    // IFD0 は縮小版で、RAW は SubIFD にある。
    expect_cfa_dng("raw_cfa_ljpeg_rggb.dng", SerColorId::BayerRGGB, false);
    const ImageFileInfo info = stackcore::probe_image_file(data_path("raw_cfa_ljpeg_rggb.dng"));
    MT_CHECK_EQ(info.bit_depth, 12);
    // EXIF の撮影時刻と時差から UTC を求める（02:24:06.25 +09:00 → 前日 17:24:06.250）。
    MT_CHECK(stackcore::ticks_to_iso8601(info.timestamp_ticks) == "2024-01-18T17:24:06.250");
}

MT_TEST(raw_DNG_詰めた12bitと線形化表と黒レベルの行列差分) {
    // ビッグエンディアン。
    expect_cfa_dng("raw_cfa12_packed_bggr.dng", SerColorId::BayerBGGR, true);
}

MT_TEST(raw_DNG_LinearRawの16bit浮動小数点とDeflateと予測子) {
    FrameBuffer f;
    ImageFileInfo info;
    stackcore::read_image_file(data_path("raw_linear_f16_deflate.dng"), f, info);
    MT_CHECK_EQ(f.width(), 40);
    MT_CHECK_EQ(f.height(), 30);
    MT_CHECK_EQ(f.channels(), 3);
    MT_CHECK(info.color == SerColorId::RGB);
    for (int c = 0; c < 3; ++c) {
        for (int y = 0; y < 30; ++y) {
            for (int x = 0; x < 40; ++x) {
                const double expected = value(x, y) / 4000.0 * (0.5 + 0.25 * c);
                // 半精度の丸め（有効数字約3桁）と、LibRaw が整数（16383段）へ直すときの切り捨て。
                MT_CHECK_NEAR(f.row(c, y)[x], std::min(1.0, expected), expected * 1e-3 + 7e-5);
            }
        }
    }
}

MT_TEST(raw_RAWの連番は撮影時刻を持つ) {
    // 同じファイルを2枚の連番として開く（ダーク・フラットの原本と同じ経路）。
    stackcore::OpenOptions options;
    options.sequence_files = {data_path("raw_cfa_ljpeg_rggb.dng"), data_path("raw_cfa_ljpeg_rggb.dng")};
    std::unique_ptr<stackcore::VideoSource> source = stackcore::open_video("", options);
    MT_CHECK_EQ(source->frame_count(), 2);
    MT_CHECK(source->color_id() == SerColorId::BayerRGGB);
    MT_CHECK(source->has_timestamps());
    MT_CHECK(stackcore::ticks_to_iso8601(source->timestamp_ticks(1)) == "2024-01-18T17:24:06.250");
    FrameBuffer frame;
    source->read_frame(1, frame);
    MT_CHECK_EQ(frame.width(), kCropW);
    MT_CHECK_EQ(frame.channels(), 1);
}

MT_TEST(raw_EXIFの日時をUTCのticksにする) {
    MT_CHECK(stackcore::ticks_to_iso8601(stackcore::exif_datetime_to_ticks("2025:07:13 22:27:54", "14", 540)) ==
             "2025-07-13T13:27:54.140");
    MT_CHECK(stackcore::ticks_to_iso8601(stackcore::exif_datetime_to_ticks("2024:03:01 00:10:00", "", 60)) ==
             "2024-02-29T23:10:00.000");
    MT_CHECK(stackcore::ticks_to_iso8601(stackcore::exif_datetime_to_ticks("2023:12:31 20:00:00", "5", -300)) ==
             "2024-01-01T01:00:00.500");
    MT_CHECK_EQ(stackcore::exif_datetime_to_ticks("", "", 0), 0);
    MT_CHECK_EQ(stackcore::exif_datetime_to_ticks("    :  :     :  :  ", "", 0), 0);
}

MT_TEST(raw_壊れたファイルや非対応の形式ははっきり失敗する) {
    FrameBuffer f;
    ImageFileInfo info;
    // TIFF（DNGVersion なし）を .dng という名前で渡す。
    {
        const std::string fake = "/tmp/lunastack_test_not_really.dng";
        std::FILE* in = std::fopen(data_path("gray16_pred_lzw_be.tif").c_str(), "rb");
        std::FILE* out = std::fopen(fake.c_str(), "wb");
        MT_CHECK(in && out);
        int ch;
        while ((ch = std::fgetc(in)) != EOF) std::fputc(ch, out);
        std::fclose(in);
        std::fclose(out);
        MT_CHECK_THROWS(stackcore::read_raw_image(fake, f, info));
        std::remove(fake.c_str());
    }
    // 拡張子がRAWでない。
    MT_CHECK_THROWS(stackcore::read_raw_image(data_path("gray16_pred_lzw_be.tif"), f, info));
}

MT_TEST(raw_連番はRAWとJPEGが混ざっていても1種類にそろえる) {
    using stackcore::select_sequence_files;
    const std::vector<std::string> mixed = {"/d/IMG_1.CR2", "/d/IMG_1.JPG", "/d/IMG_2.CR2", "/d/IMG_2.JPG",
                                            "/d/notes.txt"};
    const std::vector<std::string> raw = select_sequence_files(mixed);
    MT_CHECK_EQ(static_cast<int>(raw.size()), 2);
    MT_CHECK(raw[0] == "/d/IMG_1.CR2" && raw[1] == "/d/IMG_2.CR2");
    // RAWが無ければ多い形式（tif と tiff は同じ）。
    const std::vector<std::string> images = {"/d/a.tif", "/d/b.png", "/d/c.TIFF", "/d/d.png", "/d/e.tif"};
    const std::vector<std::string> tif = select_sequence_files(images);
    MT_CHECK_EQ(static_cast<int>(tif.size()), 3);
    MT_CHECK(tif[1] == "/d/c.TIFF");
    MT_CHECK(select_sequence_files({"/d/x.txt"}).empty());
}

MT_TEST(raw_LibRawで読む形式も壊れたファイルははっきり失敗する) {
    // 中身がRAWでない .NEF・.CR3。LibRaw が読めないと言ったら、形式名つきの例外にする。
    for (const char* name : {"/tmp/lunastack_test_broken.NEF", "/tmp/lunastack_test_broken.cr3"}) {
        std::FILE* fp = std::fopen(name, "wb");
        MT_CHECK(fp != nullptr);
        const char junk[] = "this is not a raw file at all, just some text to confuse the parser";
        for (int i = 0; i < 64; ++i) std::fwrite(junk, 1, sizeof(junk), fp);
        std::fclose(fp);
        FrameBuffer f;
        ImageFileInfo info;
        MT_CHECK_THROWS(stackcore::read_image_file(name, f, info));
        MT_CHECK_THROWS(stackcore::probe_image_file(name));
        std::remove(name);
    }
}
