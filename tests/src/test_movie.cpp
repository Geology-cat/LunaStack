// MOV・MP4 の読み込み（AVFoundation）のテスト。
//
// 試験画像は tests/data/make_movie_fixtures.sh で作った 64×48・30フレームの動画で、
// 左半分の明るさがフレーム番号 k を表す（20 + 7k）。H.264 は非可逆なので ±3 段まで許す。

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "microtest.hpp"
#include "stackcore/metadata.hpp"
#include "stackcore/video_source.hpp"

using stackcore::FrameBuffer;

namespace {

std::string data_path(const char* name) { return std::string(LUNASTACK_TEST_DATA_DIR) + "/" + name; }

// 左半分（縁を除く）の緑の平均（0〜255）。
double left_level(const FrameBuffer& f) {
    double s = 0.0;
    int n = 0;
    for (int y = 4; y < f.height() - 4; ++y) {
        for (int x = 4; x < 28; ++x) {
            s += f.row(1, y)[x] * 255.0;
            ++n;
        }
    }
    return s / n;
}

void expect_frames(const char* name, double tolerance) {
    stackcore::OpenOptions o;
    std::unique_ptr<stackcore::VideoSource> src = stackcore::open_video(data_path(name), o);
    MT_CHECK_EQ(src->width(), 64);
    MT_CHECK_EQ(src->height(), 48);
    MT_CHECK_EQ(src->frame_count(), 30);
    MT_CHECK(src->color_id() == stackcore::SerColorId::RGB);
    MT_CHECK(src->describe().find("AVFoundation") != std::string::npos);
    FrameBuffer f;
    // 番号順（B フレームがあるので表示順とデコード順が違う）。
    for (int k = 0; k < 30; ++k) {
        src->read_frame(k, f);
        MT_CHECK_NEAR(left_level(f), 20.0 + 7.0 * k, tolerance);
    }
    // ばらばらの順でも同じフレームが出る（キーフレームから読み直す経路）。
    std::vector<int> order(30);
    for (int k = 0; k < 30; ++k) order[static_cast<std::size_t>(k)] = k;
    std::mt19937 rng(7);
    std::shuffle(order.begin(), order.end(), rng);
    for (int k : order) {
        src->read_frame(k, f);
        MT_CHECK_NEAR(left_level(f), 20.0 + 7.0 * k, tolerance);
    }
}

}  // namespace

MT_TEST(movie_H264のBフレームつきMP4を番号どおりに読める) { expect_frames("movie_h264_bframes.mp4", 3.0); }

MT_TEST(movie_ProResのMOVを番号どおりに読める) { expect_frames("movie_prores.mov", 1.0); }

MT_TEST(movie_QuickTimeの作成日時からフレームごとのUTCを求める) {
    stackcore::OpenOptions o;
    std::unique_ptr<stackcore::VideoSource> src = stackcore::open_video(data_path("movie_h264_dated.mov"), o);
    MT_CHECK(src->has_timestamps());
    // 02:24:06.250 +09:00 → 前日 17:24:06.250。フレームは 1/30 秒ずつ。
    MT_CHECK(stackcore::ticks_to_iso8601(src->timestamp_ticks(0)) == "2024-01-18T17:24:06.250");
    MT_CHECK(stackcore::ticks_to_iso8601(src->timestamp_ticks(3)) == "2024-01-18T17:24:06.350");
    // 時差の無い MP4（ffmpeg の既定）は時刻を使わない。
    std::unique_ptr<stackcore::VideoSource> plain = stackcore::open_video(data_path("movie_h264_bframes.mp4"), o);
    MT_CHECK(!plain->has_timestamps());
}

MT_TEST(movie_フレーム範囲と処理範囲も掛けられる) {
    stackcore::OpenOptions o;
    o.frame_start = 10;
    o.frame_end = 20;
    o.roi_x = 0;
    o.roi_y = 0;
    o.roi_width = 32;
    o.roi_height = 48;
    std::unique_ptr<stackcore::VideoSource> src = stackcore::open_video(data_path("movie_prores.mov"), o);
    MT_CHECK_EQ(src->frame_count(), 10);
    MT_CHECK_EQ(src->width(), 32);
    FrameBuffer f;
    src->read_frame(5, f);  // 元の15枚目
    MT_CHECK_NEAR(left_level(f), 20.0 + 7.0 * 15, 1.0);
}
