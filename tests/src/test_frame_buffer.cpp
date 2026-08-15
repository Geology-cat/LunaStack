#include <cstdint>

#include "microtest.hpp"
#include "stackcore/frame_buffer.hpp"

using stackcore::FrameBuffer;

MT_TEST(frame_buffer_行が32バイト境界に揃う) {
    // 幅を意図的に半端な値にして、stride が width と別物であることを確かめる。
    const int widths[] = {1, 3, 7, 8, 9, 100, 641};
    for (int i = 0; i < 7; ++i) {
        FrameBuffer fb(widths[i], 5, 3);
        MT_CHECK(fb.stride() >= static_cast<std::size_t>(widths[i]));
        MT_CHECK_EQ((fb.stride() * sizeof(float)) % 32u, 0u);

        for (int c = 0; c < 3; ++c) {
            for (int y = 0; y < 5; ++y) {
                const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(fb.row(c, y));
                MT_CHECK_EQ(addr % 32u, 0u);
            }
        }
    }
}

MT_TEST(frame_buffer_確保直後はゼロ初期化されている) {
    FrameBuffer fb(16, 9, 3);
    for (int c = 0; c < 3; ++c) {
        for (int y = 0; y < 9; ++y) {
            const float* row = fb.row(c, y);
            for (int x = 0; x < 16; ++x) MT_CHECK_EQ(row[x], 0.0f);
        }
    }
}

MT_TEST(frame_buffer_プレーンが独立している) {
    FrameBuffer fb(8, 4, 3);
    fb.row(0, 0)[0] = 1.0f;
    fb.row(1, 0)[0] = 2.0f;
    fb.row(2, 0)[0] = 3.0f;
    MT_CHECK_EQ(fb.row(0, 0)[0], 1.0f);
    MT_CHECK_EQ(fb.row(1, 0)[0], 2.0f);
    MT_CHECK_EQ(fb.row(2, 0)[0], 3.0f);
}

MT_TEST(frame_buffer_モノクロのlumaは実体を共有しコピーしない) {
    FrameBuffer fb(8, 4, 1);
    fb.row(0, 0)[3] = 0.75f;
    MT_CHECK(fb.luma() == fb.plane(0));
    MT_CHECK_EQ(fb.luma()[3], 0.75f);
}

MT_TEST(frame_buffer_カラーのlumaはRec709で計算される) {
    FrameBuffer fb(4, 2, 3);
    fb.row(0, 0)[0] = 1.0f;  // R
    fb.row(1, 0)[0] = 0.0f;  // G
    fb.row(2, 0)[0] = 0.0f;  // B
    fb.row(0, 0)[1] = 0.0f;
    fb.row(1, 0)[1] = 1.0f;
    fb.row(2, 0)[1] = 0.0f;
    fb.row(0, 0)[2] = 0.0f;
    fb.row(1, 0)[2] = 0.0f;
    fb.row(2, 0)[2] = 1.0f;
    fb.invalidate_luma();

    const float* luma = fb.luma();
    MT_CHECK(luma != nullptr);
    MT_CHECK_NEAR(luma[0], 0.2126, 1e-6);
    MT_CHECK_NEAR(luma[1], 0.7152, 1e-6);
    MT_CHECK_NEAR(luma[2], 0.0722, 1e-6);
}

MT_TEST(frame_buffer_lumaは無効化すると再計算される) {
    FrameBuffer fb(4, 2, 3);
    fb.row(0, 0)[0] = 1.0f;
    fb.invalidate_luma();
    MT_CHECK_NEAR(fb.luma()[0], 0.2126, 1e-6);

    fb.row(0, 0)[0] = 0.0f;
    fb.row(1, 0)[0] = 1.0f;
    fb.invalidate_luma();
    MT_CHECK_NEAR(fb.luma()[0], 0.7152, 1e-6);
}

MT_TEST(frame_buffer_不正なサイズは例外になる) {
    MT_CHECK_THROWS(FrameBuffer(0, 10, 1));
    MT_CHECK_THROWS(FrameBuffer(10, 0, 1));
    MT_CHECK_THROWS(FrameBuffer(10, 10, 0));
    MT_CHECK_THROWS(FrameBuffer(-1, 10, 1));
}

MT_TEST(frame_buffer_clearでゼロに戻る) {
    FrameBuffer fb(8, 4, 1);
    fb.row(0, 2)[5] = 0.5f;
    fb.clear();
    MT_CHECK_EQ(fb.row(0, 2)[5], 0.0f);
}
