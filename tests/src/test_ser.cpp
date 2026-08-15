// SERデコーダのリグレッションテスト。
//
// 【重要】ここで使うのは自作の合成SERであり、これに通ることは
// デコーダが実ファイルを正しく読めることの証明にはならない。
// 実キャプチャソフト出力での確認がM0の受け入れ条件である（実装計画書 §5）。
// このスイートの役割は「一度正しくなった挙動を壊さないこと」に限られる。

#include <string>

#include "microtest.hpp"
#include "stackcore/ser_decoder.hpp"
#include "synthetic_ser.hpp"

using stackcore::ByteOrder;
using stackcore::FrameBuffer;
using stackcore::SerColorId;
using stackcore::SerDecoder;

namespace {

std::string temp_path(const char* name) { return std::string("/tmp/lunastack_test_") + name; }

}  // namespace

MT_TEST(ser_ヘッダを正しく読む) {
    synthetic::SerSpec spec;
    spec.width = 40;
    spec.height = 30;
    spec.frames = 7;
    spec.pixel_depth = 16;
    const std::string path = temp_path("header.ser");
    synthetic::write_ser(path, spec);

    SerDecoder d;
    d.open(path);
    MT_CHECK_EQ(d.header().file_id, std::string("LUCAM-RECORDER"));
    MT_CHECK_EQ(d.header().width, 40);
    MT_CHECK_EQ(d.header().height, 30);
    MT_CHECK_EQ(d.header().frame_count, 7);
    MT_CHECK_EQ(d.header().pixel_depth, 16);
    MT_CHECK_EQ(d.planes(), 1);
    MT_CHECK_EQ(d.bytes_per_sample(), 2);
    MT_CHECK_EQ(d.frame_bytes(), static_cast<std::size_t>(40 * 30 * 2));
    MT_CHECK(d.has_timestamps());
    MT_CHECK_EQ(d.header().observer, std::string("TestObserver"));
}

MT_TEST(ser_GenikaAstroの非標準識別子を互換入力として読める) {
    // 実ファイルで確認した方言。Genika AstroはSERの各フィールドを正しく書く一方、
    // 先頭14バイトだけ標準のLUCAM-RECORDERではなくGenikaAstroにしている。
    // 寸法・深度・必要サイズを厳密に検査した上で、識別子だけを理由に拒否しない。
    synthetic::SerSpec spec;
    spec.file_id = "GenikaAstro";
    spec.pixel_depth = 8;
    spec.width = 40;
    spec.height = 24;
    spec.frames = 3;
    const std::string path = temp_path("genika_id.ser");
    synthetic::write_ser(path, spec);

    SerDecoder d;
    d.open(path);
    MT_CHECK_EQ(d.header().file_id, std::string("GenikaAstro"));
    MT_CHECK_EQ(d.header().width, 40);
    MT_CHECK_EQ(d.header().height, 24);
    MT_CHECK_EQ(d.frame_count(), 3);

    FrameBuffer fb;
    d.read_frame(2, fb);
    const float expected =
        static_cast<float>(synthetic::synthetic_value(spec, 11, 7, 2, 0)) / 255.0f;
    MT_CHECK_NEAR(fb.row(0, 7)[11], expected, 1e-6);
}

MT_TEST(ser_16bitモノクロの画素値が一致する) {
    synthetic::SerSpec spec;
    spec.width = 33;  // stride と width をずらすため半端な幅にする
    spec.height = 17;
    spec.frames = 3;
    const std::string path = temp_path("mono16.ser");
    synthetic::write_ser(path, spec);

    SerDecoder d;
    d.open(path);
    FrameBuffer fb;
    d.read_frame(2, fb);

    MT_CHECK_EQ(fb.width(), 33);
    MT_CHECK_EQ(fb.channels(), 1);

    const float scale = 1.0f / 65535.0f;
    for (int y = 0; y < spec.height; ++y) {
        const float* row = fb.row(0, y);
        for (int x = 0; x < spec.width; ++x) {
            const float expected =
                static_cast<float>(synthetic::synthetic_value(spec, x, y, 2, 0)) * scale;
            MT_CHECK_NEAR(row[x], expected, 1e-6);
        }
    }
}

MT_TEST(ser_8bitは深度255で正規化される) {
    synthetic::SerSpec spec;
    spec.pixel_depth = 8;
    spec.width = 16;
    spec.height = 8;
    const std::string path = temp_path("mono8.ser");
    synthetic::write_ser(path, spec);

    SerDecoder d;
    d.open(path);
    MT_CHECK_EQ(d.bytes_per_sample(), 1);

    FrameBuffer fb;
    d.read_frame(0, fb);
    const float expected =
        static_cast<float>(synthetic::synthetic_value(spec, 5, 3, 0, 0)) / 255.0f;
    MT_CHECK_NEAR(fb.row(0, 3)[5], expected, 1e-6);
}

MT_TEST(ser_RGBは3プレーンに分離される) {
    synthetic::SerSpec spec;
    spec.color_id = 100;  // RGB
    spec.width = 12;
    spec.height = 6;
    const std::string path = temp_path("rgb.ser");
    synthetic::write_ser(path, spec);

    SerDecoder d;
    d.open(path);
    MT_CHECK_EQ(d.planes(), 3);

    FrameBuffer fb;
    d.read_frame(0, fb);
    MT_CHECK_EQ(fb.channels(), 3);

    const float scale = 1.0f / 65535.0f;
    for (int c = 0; c < 3; ++c) {
        const float expected =
            static_cast<float>(synthetic::synthetic_value(spec, 4, 2, 0, c)) * scale;
        MT_CHECK_NEAR(fb.row(c, 2)[4], expected, 1e-6);
    }
}

MT_TEST(ser_ヘッダのバイトオーダーが嘘でも自動判定で正しく読める) {
    // 実運用で最も多い罠: ヘッダは little と書いてあるが中身は big。
    // 判定に失敗すると画像はノイズになる（実装計画書 §4.8）。
    synthetic::SerSpec spec;
    spec.width = 64;
    spec.height = 48;
    spec.frames = 3;
    spec.header_says_little = true;
    spec.data_is_little = false;  // ヘッダの主張と食い違わせる
    const std::string path = temp_path("endian_lie.ser");
    synthetic::write_ser(path, spec);

    SerDecoder d;
    d.open(path, ByteOrder::Auto);
    MT_CHECK(d.resolved_byte_order() == ByteOrder::Big);
    MT_CHECK(d.byte_order_differs_from_header());

    FrameBuffer fb;
    d.read_frame(1, fb);
    const float expected =
        static_cast<float>(synthetic::synthetic_value(spec, 10, 20, 1, 0)) / 65535.0f;
    MT_CHECK_NEAR(fb.row(0, 20)[10], expected, 1e-6);
}

MT_TEST(ser_増分が256の倍数になる無ノイズ画像でも自動判定を誤らない) {
    // リグレッション: 水平方向の差だけで判定していたときに実際に誤判定したケース。
    // 幅33・16bitのランプは1画素あたりちょうど1024(0x0400)ずつ増えるため、
    // バイトを入れ替えると下位バイトが変化せず上位バイトだけが+4される。
    // つまり「入れ替えた方が水平方向には滑らか」に見えてしまう。
    // 垂直方向も同時に見ることで正しく判定できる。
    synthetic::SerSpec spec;
    spec.width = 33;
    spec.height = 17;
    spec.frames = 3;
    spec.pixel_depth = 16;
    spec.with_noise = false;  // 最悪ケースを意図的に再現する
    spec.header_says_little = true;
    spec.data_is_little = true;
    const std::string path = temp_path("ramp_256step.ser");
    synthetic::write_ser(path, spec);

    SerDecoder d;
    d.open(path, ByteOrder::Auto);
    MT_CHECK(d.resolved_byte_order() == ByteOrder::Little);

    FrameBuffer fb;
    d.read_frame(1, fb);
    const float expected =
        static_cast<float>(synthetic::synthetic_value(spec, 0, 0, 1, 0)) / 65535.0f;
    MT_CHECK_NEAR(fb.row(0, 0)[0], expected, 1e-6);
}

MT_TEST(ser_バイトオーダーを手動で上書きできる) {
    synthetic::SerSpec spec;
    spec.width = 64;
    spec.height = 48;
    spec.header_says_little = false;
    spec.data_is_little = true;
    const std::string path = temp_path("endian_manual.ser");
    synthetic::write_ser(path, spec);

    SerDecoder d;
    d.open(path, ByteOrder::Little);
    MT_CHECK(d.resolved_byte_order() == ByteOrder::Little);

    FrameBuffer fb;
    d.read_frame(0, fb);
    const float expected =
        static_cast<float>(synthetic::synthetic_value(spec, 30, 10, 0, 0)) / 65535.0f;
    MT_CHECK_NEAR(fb.row(0, 10)[30], expected, 1e-6);
}

MT_TEST(ser_ビット深度の上書きが正規化に反映される) {
    // 「16bitと書いてあるが中身は12bit」への対処手段が効くこと。
    synthetic::SerSpec spec;
    spec.pixel_depth = 16;
    spec.width = 8;
    spec.height = 8;
    const std::string path = temp_path("depth_override.ser");
    synthetic::write_ser(path, spec);

    SerDecoder d;
    d.open(path);
    FrameBuffer normal;
    d.read_frame(0, normal);

    d.set_bit_depth_override(12);
    MT_CHECK_EQ(d.effective_bit_depth(), 12);
    FrameBuffer overridden;
    d.read_frame(0, overridden);

    // 同じ生値をより小さい最大値で割るので、値は必ず大きくなる。
    MT_CHECK(overridden.row(0, 4)[4] > normal.row(0, 4)[4]);
    MT_CHECK_EQ(overridden.source_bit_depth(), 12);
}

MT_TEST(ser_統計値が生サンプルの範囲を返す) {
    synthetic::SerSpec spec;
    spec.width = 20;
    spec.height = 10;
    const std::string path = temp_path("stats.ser");
    synthetic::write_ser(path, spec);

    SerDecoder d;
    d.open(path);
    const stackcore::FrameStats s = d.frame_stats(0);

    const std::uint32_t expected_min = synthetic::synthetic_value(spec, 0, 0, 0, 0);
    const std::uint32_t expected_max =
        synthetic::synthetic_value(spec, spec.width - 1, spec.height - 1, 0, 0);
    MT_CHECK_EQ(s.min_value, expected_min);
    MT_CHECK_EQ(s.max_value, expected_max);
    MT_CHECK(s.mean_value > static_cast<double>(expected_min));
    MT_CHECK(s.mean_value < static_cast<double>(expected_max));
}

MT_TEST(ser_タイムスタンプが単調増加で読める) {
    synthetic::SerSpec spec;
    spec.frames = 4;
    const std::string path = temp_path("timestamps.ser");
    synthetic::write_ser(path, spec);

    SerDecoder d;
    d.open(path);
    MT_CHECK(d.has_timestamps());
    for (int i = 1; i < 4; ++i) {
        MT_CHECK(d.timestamp_ticks(i) > d.timestamp_ticks(i - 1));
    }
}

MT_TEST(ser_タイムスタンプなしのファイルを検出する) {
    synthetic::SerSpec spec;
    spec.with_timestamps = false;
    const std::string path = temp_path("no_timestamps.ser");
    synthetic::write_ser(path, spec);

    SerDecoder d;
    d.open(path);
    MT_CHECK(!d.has_timestamps());
    MT_CHECK_THROWS(d.timestamp_ticks(0));
}

MT_TEST(ser_途中で切れたファイルは開けない) {
    synthetic::SerSpec spec;
    spec.width = 32;
    spec.height = 32;
    spec.frames = 10;
    const std::string full = temp_path("full.ser");
    const std::string cut = temp_path("truncated.ser");
    synthetic::write_ser(full, spec);
    synthetic::write_truncated(full, cut, 178 + 32 * 32 * 2 * 3);  // 10フレーム中3枚分だけ

    SerDecoder d;
    MT_CHECK_THROWS(d.open(cut));
}

MT_TEST(ser_ヘッダより短いファイルは開けない) {
    synthetic::SerSpec spec;
    const std::string full = temp_path("short_src.ser");
    const std::string cut = temp_path("short.ser");
    synthetic::write_ser(full, spec);
    synthetic::write_truncated(full, cut, 100);

    SerDecoder d;
    MT_CHECK_THROWS(d.open(cut));
}

MT_TEST(ser_範囲外のフレーム番号は例外になる) {
    synthetic::SerSpec spec;
    spec.frames = 3;
    const std::string path = temp_path("range.ser");
    synthetic::write_ser(path, spec);

    SerDecoder d;
    d.open(path);
    FrameBuffer fb;
    MT_CHECK_THROWS(d.read_frame(3, fb));
    MT_CHECK_THROWS(d.read_frame(-1, fb));
}

// 不正・悪意あるヘッダで落ちないこと。
// SERでないファイルを掴まされるとヘッダの各フィールドは事実上ランダムな値になる。
namespace {

// 178バイトのヘッダを直接組み立てて書き出す（合成SER生成器を通さない）。
void write_raw_header(const std::string& path, std::int32_t color_id, std::int32_t width,
                      std::int32_t height, std::int32_t depth, std::int32_t frames,
                      std::size_t payload_bytes) {
    std::vector<std::uint8_t> b;
    const char* id = "LUCAM-RECORDER";
    for (int i = 0; i < 14; ++i) b.push_back(static_cast<std::uint8_t>(id[i]));
    const std::int32_t fields[] = {0, color_id, 1, width, height, depth, frames};
    for (int f = 0; f < 7; ++f) {
        const std::uint32_t v = static_cast<std::uint32_t>(fields[f]);
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
    while (b.size() < 178) b.push_back(0);
    for (std::size_t i = 0; i < payload_bytes; ++i) b.push_back(0);

    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (fp == nullptr) throw std::runtime_error("テスト用ファイルを作成できません");
    std::fwrite(b.data(), 1, b.size(), fp);
    std::fclose(fp);
}

}  // namespace

MT_TEST(ser_巨大な画像サイズでも桁溢れせず例外になる) {
    // リグレッション: width=height=2^30, depth=16, frames=8 とすると
    // frame_bytes * frame_count が 2^64 で一周して0になり、
    // 「必要バイト数=178」と誤認してサイズ検証をすり抜けていた。
    // その結果 frame_stats() がマップ範囲外を読んでSIGSEGVで落ちた。
    const std::string path = temp_path("overflow.ser");
    write_raw_header(path, 0, 1073741824, 1073741824, 16, 8, 512);

    SerDecoder d;
    MT_CHECK_THROWS(d.open(path));
}

MT_TEST(ser_画像サイズがゼロや負でも例外になる) {
    SerDecoder d;
    const std::string p1 = temp_path("zero_w.ser");
    write_raw_header(p1, 0, 0, 100, 16, 1, 512);
    MT_CHECK_THROWS(d.open(p1));

    const std::string p2 = temp_path("neg_h.ser");
    write_raw_header(p2, 0, 100, -5, 16, 1, 512);
    MT_CHECK_THROWS(d.open(p2));
}

MT_TEST(ser_フレーム数が負や巨大でも例外になる) {
    SerDecoder d;
    const std::string p1 = temp_path("neg_frames.ser");
    write_raw_header(p1, 0, 64, 64, 16, -1, 512);
    MT_CHECK_THROWS(d.open(p1));

    const std::string p2 = temp_path("huge_frames.ser");
    write_raw_header(p2, 0, 64, 64, 16, 2000000000, 512);
    MT_CHECK_THROWS(d.open(p2));
}

MT_TEST(ser_ビット深度が範囲外なら例外になる) {
    SerDecoder d;
    const std::string p1 = temp_path("depth0.ser");
    write_raw_header(p1, 0, 64, 64, 0, 1, 512);
    MT_CHECK_THROWS(d.open(p1));

    const std::string p2 = temp_path("depth99.ser");
    write_raw_header(p2, 0, 64, 64, 99, 1, 512);
    MT_CHECK_THROWS(d.open(p2));
}

MT_TEST(ser_サイズちょうどのファイルは開けタイムスタンプなしと判定される) {
    // 境界条件: 画素データちょうどで終わるファイル。
    const std::string path = temp_path("exact.ser");
    write_raw_header(path, 0, 8, 4, 16, 3, 8u * 4u * 2u * 3u);

    SerDecoder d;
    d.open(path);
    MT_CHECK_EQ(d.frame_count(), 3);
    MT_CHECK(!d.has_timestamps());
}

MT_TEST(ser_存在しないファイルは例外になる) {
    SerDecoder d;
    MT_CHECK_THROWS(d.open("./この名前のファイルは存在しない.ser"));
}

MT_TEST(ser_カラーID判定) {
    MT_CHECK(stackcore::is_bayer(SerColorId::BayerRGGB));
    MT_CHECK(stackcore::is_bayer(SerColorId::BayerCYYM));
    MT_CHECK(!stackcore::is_bayer(SerColorId::Mono));
    MT_CHECK(!stackcore::is_bayer(SerColorId::RGB));
    MT_CHECK(stackcore::is_supported_bayer(SerColorId::BayerBGGR));
    MT_CHECK(!stackcore::is_supported_bayer(SerColorId::BayerCYYM));
}
