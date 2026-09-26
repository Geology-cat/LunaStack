// カメラのRAW（CR2・DNG）とロスレスJPEGの読み込みのテスト。
//
// 試験画像（tests/data/raw_*）は tests/data/make_raw_fixtures.py で作った小さなファイルで、
// 画素値はそこに書いた式 value(x, y) で決まっている。LibRaw でも同じ値に読めることを
// 確かめてある（実機の CR2 は LibRaw と全画素一致を確認済み。開発記録 §8）。

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "microtest.hpp"
#include "stackcore/image_reader.hpp"
#include "stackcore/lossless_jpeg.hpp"
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
constexpr int kAreaTop = 3, kAreaLeft = 2;
constexpr int kCropX = kAreaLeft + 1, kCropY = kAreaTop + 2;  // ActiveArea + DefaultCropOrigin
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
            MT_CHECK_NEAR(f.row(0, y)[x], std::min(1.0, std::max(0.0, (v - b) / (w - b))), 1e-6);
        }
    }
}

}  // namespace

MT_TEST(raw_拡張子でRAWを判定し静止画として扱う) {
    MT_CHECK(stackcore::is_raw_image_path("/a/b/IMG_0001.CR2"));
    MT_CHECK(stackcore::is_raw_image_path("/a/静止画cr2/20250713_0070345 .CR2"));
    MT_CHECK(stackcore::is_raw_image_path("x.dng"));
    MT_CHECK(!stackcore::is_raw_image_path("/a/静止画cr2/readme"));  // フォルダ名に cr2 があっても違う
    MT_CHECK(!stackcore::is_raw_image_path("x.cr3"));
    MT_CHECK(stackcore::is_supported_image_path("IMG_1.CR2"));
    MT_CHECK(stackcore::is_supported_image_path("IMG_1.DNG"));
}

MT_TEST(raw_DNG_16bit無圧縮のCFAをActiveAreaとDefaultCropで切り抜く) {
    // CFAPattern GRBG を ActiveArea の左上 (2,3) から数え、切り抜きの左上 (3,5) では
    // (1,2) ずれるので RGGB になる。
    expect_cfa_dng("raw_cfa16_grbg.dng", SerColorId::BayerRGGB, false);
}

MT_TEST(raw_DNG_ロスレスJPEGのタイルとSubIFDを読む) {
    // RGGB を (1,2) ずらすと GRBG。IFD0 は縮小版で、RAW は SubIFD にある。
    expect_cfa_dng("raw_cfa_ljpeg_rggb.dng", SerColorId::BayerGRBG, false);
    const ImageFileInfo info = stackcore::probe_image_file(data_path("raw_cfa_ljpeg_rggb.dng"));
    MT_CHECK_EQ(info.bit_depth, 12);
    // EXIF の撮影時刻と時差から UTC を求める（02:24:06.25 +09:00 → 前日 17:24:06.250）。
    MT_CHECK(stackcore::ticks_to_iso8601(info.timestamp_ticks) == "2024-01-18T17:24:06.250");
}

MT_TEST(raw_DNG_詰めた12bitと線形化表と黒レベルの行列差分) {
    // BGGR を (1,2) ずらすと GBRG。ビッグエンディアン。
    expect_cfa_dng("raw_cfa12_packed_bggr.dng", SerColorId::BayerGBRG, true);
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
                // 半精度の丸め（有効数字約3桁）。
                MT_CHECK_NEAR(f.row(c, y)[x], std::min(1.0, expected), expected * 1e-3 + 1e-6);
            }
        }
    }
}

MT_TEST(raw_CR2_スライスを戻しSensorInfoで切り抜き遮光部で黒を引く) {
    const stackcore::RawSensorData raw = stackcore::read_cr2_sensor_data(data_path("raw_sliced.cr2"));
    MT_CHECK_EQ(raw.width, 44);
    MT_CHECK_EQ(raw.height, 26);
    MT_CHECK_EQ(raw.bits, 14);
    const double black[4] = {512, 516, 520, 524};
    for (int p = 0; p < 4; ++p) MT_CHECK_NEAR(raw.black[p], black[p], 1e-9);
    // 生の値（スライスを戻した後）。
    for (int y = 0; y < 26; ++y) {
        for (int x = 0; x < 44; ++x) {
            const bool inside = x >= 20 && x <= 41 && y >= 3 && y <= 24;
            const int expected = static_cast<int>(black[(y & 1) * 2 + (x & 1)]) + (inside ? value(x, y) : 0);
            MT_CHECK_EQ(static_cast<int>(raw.values[static_cast<std::size_t>(y) * 44 + x]), expected);
        }
    }

    FrameBuffer f;
    ImageFileInfo info;
    stackcore::read_image_file(data_path("raw_sliced.cr2"), f, info);
    MT_CHECK_EQ(f.width(), 22);
    MT_CHECK_EQ(f.height(), 22);
    // RGGB を上に奇数（3）画素切ったので GBRG。
    MT_CHECK(info.color == SerColorId::BayerGBRG);
    MT_CHECK(info.format == "CR2");
    MT_CHECK(info.camera == "Canon Test Body");
    for (int y = 0; y < 22; ++y) {
        for (int x = 0; x < 22; ++x) {
            const int rx = 20 + x, ry = 3 + y;
            const double b = black[(ry & 1) * 2 + (rx & 1)];
            MT_CHECK_NEAR(f.row(0, y)[x], value(rx, ry) / (16383.0 - b), 1e-6);
        }
    }
    // 撮影時刻: EXIF は現地時刻、時差はメーカーノートの TimeInfo（+540分）。
    MT_CHECK(stackcore::ticks_to_iso8601(info.timestamp_ticks) == "2025-07-13T13:27:54.140");
}

MT_TEST(raw_CR2の連番は撮影時刻を持ち拡張子の大文字小文字を問わない) {
    // 同じファイルを2枚の連番として開く（ダーク・フラットの原本と同じ経路）。
    stackcore::OpenOptions options;
    options.sequence_files = {data_path("raw_sliced.cr2"), data_path("raw_sliced.cr2")};
    std::unique_ptr<stackcore::VideoSource> source = stackcore::open_video("", options);
    MT_CHECK_EQ(source->frame_count(), 2);
    MT_CHECK(source->color_id() == SerColorId::BayerGBRG);
    MT_CHECK(source->has_timestamps());
    MT_CHECK(stackcore::ticks_to_iso8601(source->timestamp_ticks(1)) == "2025-07-13T13:27:54.140");
    FrameBuffer frame;
    source->read_frame(1, frame);
    MT_CHECK_EQ(frame.width(), 22);
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
    // 途中で切れたロスレスJPEG。
    const std::vector<std::uint8_t> truncated = {0xFF, 0xD8, 0xFF, 0xC3, 0x00, 0x0B, 0x0E, 0x00};
    MT_CHECK_THROWS(stackcore::decode_lossless_jpeg(truncated.data(), truncated.size()));
    // ロスレスではないJPEG（SOF0）。
    const std::vector<std::uint8_t> baseline = {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x01,
                                                0x00, 0x01, 0x01, 0x01, 0x11, 0x00};
    MT_CHECK_THROWS(stackcore::decode_lossless_jpeg(baseline.data(), baseline.size()));
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
