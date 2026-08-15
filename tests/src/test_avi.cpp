#include <cmath>
#include <cstdio>
#include <string>

#include "microtest.hpp"
#include "stackcore/avi_decoder.hpp"
#include "stackcore/frame_buffer.hpp"
#include "synthetic_avi.hpp"

using stackcore::AviDecoder;
using stackcore::FrameBuffer;
using stackcore::SerColorId;

std::vector<std::uint8_t> lunastack_test_minimal_color_jpeg();
std::vector<std::uint8_t> lunastack_test_minimal_gray_jpeg();

namespace {

// 期待値（0..250の整数）を 0..1 正規化した値に直す。
float expected_norm(int frame, int x, int y, int channel, int bits) {
    const int v = synthetic_avi::expected_sample(frame, x, y, channel);
    return bits == 16 ? static_cast<float>(v * 257) / 65535.0f
                      : static_cast<float>(v) / 255.0f;
}

// 全フレーム・全画素を期待値と突き合わせる。
void check_all(AviDecoder& d, const synthetic_avi::Options& o, int frames) {
    FrameBuffer f;
    for (int n = 0; n < frames; ++n) {
        d.read_frame(n, f);
        MT_CHECK_EQ(f.width(), o.width);
        MT_CHECK_EQ(f.height(), o.height);
        for (int y = 0; y < o.height; ++y) {
            for (int x = 0; x < o.width; ++x) {
                for (int c = 0; c < f.channels(); ++c) {
                    const float got = f.row(c, y)[x];
                    const float want = expected_norm(n, x, y, c, o.bit_count);
                    if (std::fabs(got - want) > 1e-5f) {
                        microtest::fail(
                            "frame " + microtest::mt_str(n) + " (" + microtest::mt_str(x) + "," +
                            microtest::mt_str(y) + ") ch" + microtest::mt_str(c) +
                            ": 期待 " + microtest::mt_str(want) + " 実際 " + microtest::mt_str(got));
                        return;
                    }
                }
            }
        }
    }
}

std::string make(const synthetic_avi::Options& o, int frames, const char* name) {
    return synthetic_avi::write_temp(synthetic_avi::build(o, frames), name);
}

}  // namespace

MT_TEST(avi_BI_RGB24のボトムアップを正しい向きで読む) {
    // biHeightが正のBI_RGBは「下から上」。ここを取り違えると上下が反転する。
    synthetic_avi::Options o;
    o.width = 16;
    o.height = 12;
    o.bit_count = 24;
    const std::string path = make(o, 3, "bgr_bottomup");

    AviDecoder d;
    d.open(path);
    MT_CHECK_EQ(d.frame_count(), 3);
    MT_CHECK(d.color_id() == SerColorId::BGR);
    MT_CHECK(!d.header().top_down);
    check_all(d, o, 3);
    std::remove(path.c_str());
}

MT_TEST(avi_biHeightが負なら上から下として読む) {
    synthetic_avi::Options o;
    o.bit_count = 24;
    o.negative_height = true;
    const std::string path = make(o, 2, "bgr_topdown");

    AviDecoder d;
    d.open(path);
    MT_CHECK(d.header().top_down);
    MT_CHECK_EQ(d.header().height, 12);  // 高さは絶対値
    check_all(d, o, 2);
    std::remove(path.c_str());
}

MT_TEST(avi_FourCC形式はbiHeightが正でも上から下として読む) {
    // 「下から上」はDIB固有の規約であり、FourCC形式には適用されない。
    // ffmpegは Y800 を biHeight 正で書き、上から下として読む。
    synthetic_avi::Options o;
    o.bit_count = 8;
    o.compression = synthetic_avi::fourcc("Y800");
    o.negative_height = false;
    const std::string path = make(o, 2, "y800");

    AviDecoder d;
    d.open(path);
    MT_CHECK(d.header().top_down);
    MT_CHECK(d.color_id() == SerColorId::Mono);
    MT_CHECK_EQ(d.bit_depth(), 8);
    check_all(d, o, 2);
    std::remove(path.c_str());
}

MT_TEST(avi_16bitモノクロを読む) {
    synthetic_avi::Options o;
    o.bit_count = 16;
    o.compression = synthetic_avi::fourcc("Y16 ");
    const std::string path = make(o, 2, "y16");

    AviDecoder d;
    d.open(path);
    MT_CHECK_EQ(d.bit_depth(), 16);
    MT_CHECK(d.color_id() == SerColorId::Mono);
    check_all(d, o, 2);
    std::remove(path.c_str());
}

MT_TEST(avi_行のパディングを正しく飛ばす) {
    // 幅17の24bitは1行51バイト。DWORD境界では52バイトに切り上がる。
    // パディングを飛ばし損ねると2行目以降が1バイトずつずれる。
    synthetic_avi::Options o;
    o.width = 17;
    o.height = 5;
    o.bit_count = 24;
    o.pad_rows = true;
    const std::string path = make(o, 2, "padded");

    AviDecoder d;
    d.open(path);
    MT_CHECK_EQ(static_cast<int>(d.header().row_bytes), 52);
    check_all(d, o, 2);
    std::remove(path.c_str());
}

MT_TEST(avi_パディングなしで書かれたファイルも読む) {
    synthetic_avi::Options o;
    o.width = 17;
    o.height = 5;
    o.bit_count = 24;
    o.pad_rows = false;
    const std::string path = make(o, 2, "unpadded");

    AviDecoder d;
    d.open(path);
    MT_CHECK_EQ(static_cast<int>(d.header().row_bytes), 51);
    check_all(d, o, 2);
    std::remove(path.c_str());
}

MT_TEST(avi_OpenDMLの複数RIFFセグメントを跨いで全フレームを見つける) {
    // 2GBを超えるAVIはこの構造になる。先頭のRIFFだけ見ると
    // 最初のセグメント分しかフレームが見つからない。
    synthetic_avi::Options o;
    o.bit_count = 24;
    o.frames_per_segment = 3;
    const std::string path = make(o, 10, "opendml");

    AviDecoder d;
    d.open(path);
    MT_CHECK_EQ(d.frame_count(), 10);
    check_all(d, o, 10);
    std::remove(path.c_str());
}

MT_TEST(avi_recで束ねられたフレームを見つける) {
    synthetic_avi::Options o;
    o.bit_count = 24;
    o.group_in_rec = true;
    const std::string path = make(o, 4, "rec");

    AviDecoder d;
    d.open(path);
    MT_CHECK_EQ(d.frame_count(), 4);
    check_all(d, o, 4);
    std::remove(path.c_str());
}

MT_TEST(avi_MJPEGのベースラインJPEGを読む) {
    synthetic_avi::Options o;
    o.width = 8;
    o.height = 8;
    o.bit_count = 24;
    o.compression = synthetic_avi::fourcc("MJPG");
    o.frame_payload_override = lunastack_test_minimal_color_jpeg();
    const std::string path = make(o, 2, "mjpg");

    AviDecoder d;
    d.open(path);
    MT_CHECK(d.is_mjpeg());
    MT_CHECK(d.color_id() == SerColorId::RGB);
    MT_CHECK_EQ(d.frame_count(), 2);
    FrameBuffer frame;
    d.read_frame(1, frame);
    MT_CHECK_EQ(frame.channels(), 3);
    MT_CHECK_NEAR(frame.row(0, 0)[0], 128.0 / 255.0, 1e-7);
    const stackcore::FrameStats stats = d.frame_stats(0);
    MT_CHECK_EQ(static_cast<int>(stats.min_value), 128);
    MT_CHECK_EQ(static_cast<int>(stats.max_value), 128);
    std::remove(path.c_str());
}

MT_TEST(avi_MJPEGのグレースケールを1chとして読む) {
    synthetic_avi::Options o;
    o.width = 8;
    o.height = 8;
    o.bit_count = 8;
    o.compression = synthetic_avi::fourcc("MJPG");
    o.frame_payload_override = lunastack_test_minimal_gray_jpeg();
    const std::string path = make(o, 1, "mjpg_gray");

    AviDecoder d;
    d.open(path);
    MT_CHECK(d.color_id() == SerColorId::Mono);
    MT_CHECK_EQ(d.planes(), 1);
    MT_CHECK_EQ(d.header().bit_count, 8);
    FrameBuffer frame;
    d.read_frame(0, frame);
    MT_CHECK_EQ(frame.channels(), 1);
    std::remove(path.c_str());
}

MT_TEST(avi_未対応の圧縮形式は黙って読まずに拒否する) {
    synthetic_avi::Options o;
    o.bit_count = 24;
    o.compression = synthetic_avi::fourcc("H264");
    const std::string path = make(o, 2, "unsupported");

    AviDecoder d;
    MT_CHECK_THROWS(d.open(path));
    std::remove(path.c_str());
}

MT_TEST(avi_AVIでないファイルは拒否する) {
    const std::string path = "/tmp/lunastack_test_notavi.avi";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    const char junk[] = "これはAVIではありません。十分な長さのダミーデータ。";
    std::fwrite(junk, 1, sizeof(junk), f);
    std::fclose(f);

    AviDecoder d;
    MT_CHECK_THROWS(d.open(path));
    std::remove(path.c_str());
}

MT_TEST(avi_範囲外のフレーム番号は例外になる) {
    synthetic_avi::Options o;
    o.bit_count = 24;
    const std::string path = make(o, 3, "range");

    AviDecoder d;
    d.open(path);
    FrameBuffer f;
    MT_CHECK_THROWS(d.read_frame(3, f));
    MT_CHECK_THROWS(d.read_frame(-1, f));
    std::remove(path.c_str());
}

MT_TEST(avi_フレームレートを読む) {
    synthetic_avi::Options o;
    o.bit_count = 24;
    const std::string path = make(o, 2, "fps");

    AviDecoder d;
    d.open(path);
    MT_CHECK_NEAR(d.header().fps, 30.0, 1e-9);
    std::remove(path.c_str());
}
