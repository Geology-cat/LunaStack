// 仕上げ工程（チャンネル合わせ・色・形・デリンギング・処理系全体）のテスト。

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "microtest.hpp"
#include "stackcore/finishing.hpp"
#include "stackcore/resample.hpp"

using stackcore::ChannelOffsets;
using stackcore::FinishingPipeline;
using stackcore::FinishingSettings;
using stackcore::FrameBuffer;
using stackcore::Geometry;
using stackcore::apply_geometry;
using stackcore::crop_frame;
using stackcore::geometry_output_rect_to_input;

namespace {

// 縞模様のある円盤（惑星を模す）。黒い背景の上に置く。
double planet(double x, double y, double cx, double cy, double r) {
    const double d = std::hypot(x - cx, y - cy);
    if (d > r + 1.5) return 0.02;
    const double edge = std::min(1.0, std::max(0.0, (r + 1.5 - d) / 3.0));
    const double bands = 0.55 + 0.2 * std::sin(y * 0.45) + 0.1 * std::sin((x + 2 * y) * 0.31);
    return 0.02 + edge * bands;
}

// R・Bが既知の量だけずれたカラー惑星。
FrameBuffer dispersed_planet(int w, int h, const ChannelOffsets& shift) {
    FrameBuffer f(w, h, 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // 出力(x) = 入力(x + dx) で戻せるよう、チャンネルcは (x - dx) の位置の模様を持つ。
            f.row(0, y)[x] = static_cast<float>(0.9 * planet(x - shift.red_dx, y - shift.red_dy, w / 2.0, h / 2.0, w / 4.0));
            f.row(1, y)[x] = static_cast<float>(planet(x, y, w / 2.0, h / 2.0, w / 4.0));
            f.row(2, y)[x] = static_cast<float>(0.8 * planet(x - shift.blue_dx, y - shift.blue_dy, w / 2.0, h / 2.0, w / 4.0));
        }
    }
    return f;
}

bool same_bytes(const FrameBuffer& a, const FrameBuffer& b) {
    if (a.width() != b.width() || a.height() != b.height() || a.channels() != b.channels()) return false;
    for (int c = 0; c < a.channels(); ++c) {
        for (int y = 0; y < a.height(); ++y) {
            if (std::memcmp(a.row(c, y), b.row(c, y), sizeof(float) * a.width()) != 0) return false;
        }
    }
    return true;
}

}  // namespace

MT_TEST(finishing_チャンネルずれを推定して戻せる) {
    ChannelOffsets truth;
    truth.red_dx = 1.5;
    truth.red_dy = -0.75;
    truth.blue_dx = -1.25;
    truth.blue_dy = 2.0;
    const FrameBuffer img = dispersed_planet(160, 160, truth);
    const ChannelOffsets est = stackcore::estimate_channel_offsets(img);
    MT_CHECK_NEAR(est.red_dx, truth.red_dx, 0.15);
    MT_CHECK_NEAR(est.red_dy, truth.red_dy, 0.15);
    MT_CHECK_NEAR(est.blue_dx, truth.blue_dx, 0.15);
    MT_CHECK_NEAR(est.blue_dy, truth.blue_dy, 0.15);

    // 戻した画像はずれていない（再推定がほぼ0）。
    FrameBuffer fixed;
    stackcore::shift_channels(img, est, fixed);
    const ChannelOffsets again = stackcore::estimate_channel_offsets(fixed);
    MT_CHECK_NEAR(again.red_dx, 0.0, 0.15);
    MT_CHECK_NEAR(again.blue_dy, 0.0, 0.15);

    // モノクロや変位0なら何もしない。
    FrameBuffer mono(32, 32, 1);
    MT_CHECK(!stackcore::estimate_channel_offsets(mono).any());
    FrameBuffer copy;
    stackcore::shift_channels(img, ChannelOffsets(), copy);
    MT_CHECK(same_bytes(copy, img));
}

MT_TEST(finishing_ホワイトバランスと彩度) {
    FrameBuffer img(40, 40, 3);
    for (int y = 0; y < 40; ++y) {
        for (int x = 0; x < 40; ++x) {
            const bool object = std::hypot(x - 20.0, y - 20.0) < 12.0;
            const float v = object ? 0.5f : 0.01f;
            img.row(0, y)[x] = v * 1.25f;  // 赤っぽく写っている
            img.row(1, y)[x] = v;
            img.row(2, y)[x] = v * 0.8f;
        }
    }
    double gains[3];
    stackcore::estimate_white_balance(img, gains);
    MT_CHECK_NEAR(gains[0], 0.8, 0.002);
    MT_CHECK_NEAR(gains[1], 1.0, 1e-12);
    MT_CHECK_NEAR(gains[2], 1.25, 0.002);

    stackcore::ColorAdjust adjust;
    adjust.gain[0] = gains[0];
    adjust.gain[2] = gains[2];
    FrameBuffer balanced;
    stackcore::apply_color(img, adjust, balanced);
    MT_CHECK_NEAR(balanced.row(0, 20)[20], balanced.row(1, 20)[20], 0.002);
    MT_CHECK_NEAR(balanced.row(2, 20)[20], balanced.row(1, 20)[20], 0.002);

    stackcore::ColorAdjust gray;
    gray.saturation = 0.0;
    FrameBuffer mono;
    stackcore::apply_color(img, gray, mono);
    MT_CHECK_NEAR(mono.row(0, 20)[20], mono.row(2, 20)[20], 1e-6);
}

MT_TEST(finishing_回転反転クロップ) {
    FrameBuffer img(5, 3, 1);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 5; ++x) img.row(0, y)[x] = static_cast<float>(y * 10 + x);
    }
    stackcore::Geometry g;
    g.rotate_quarter_turns = 1;  // 時計回り90°
    FrameBuffer r;
    stackcore::apply_geometry(img, g, r);
    MT_CHECK_EQ(r.width(), 3);
    MT_CHECK_EQ(r.height(), 5);
    // 時計回りに回すと、元の左下が左上に、元の左上が右上に来る
    MT_CHECK_EQ(r.row(0, 0)[0], img.row(0, 2)[0]);
    MT_CHECK_EQ(r.row(0, 0)[2], img.row(0, 0)[0]);
    MT_CHECK_EQ(r.row(0, 4)[2], img.row(0, 0)[4]);

    // 90°を4回で元に戻る
    FrameBuffer cur;
    stackcore::apply_geometry(img, stackcore::Geometry(), cur);
    for (int i = 0; i < 4; ++i) {
        FrameBuffer next;
        stackcore::apply_geometry(cur, g, next);
        cur = std::move(next);
    }
    MT_CHECK(same_bytes(cur, img));

    stackcore::Geometry flip;
    flip.flip_horizontal = true;
    stackcore::apply_geometry(img, flip, r);
    MT_CHECK_EQ(r.row(0, 1)[0], img.row(0, 1)[4]);

    stackcore::Geometry crop;
    crop.crop = true;
    crop.crop_x = 1;
    crop.crop_y = 1;
    crop.crop_width = 3;
    crop.crop_height = 2;
    stackcore::apply_geometry(img, crop, r);
    MT_CHECK_EQ(r.width(), 3);
    MT_CHECK_EQ(r.height(), 2);
    MT_CHECK_EQ(r.row(0, 0)[0], img.row(0, 1)[1]);
}

MT_TEST(finishing_対象を囲む矩形を見つける) {
    FrameBuffer img(100, 80, 1);
    for (int y = 0; y < 80; ++y) {
        for (int x = 0; x < 100; ++x) {
            img.row(0, y)[x] = (x >= 30 && x < 60 && y >= 20 && y < 45) ? 0.8f : 0.02f;
        }
    }
    img.row(0, 5)[95] = 1.0f;  // ホットピクセル1つでは広がらない
    int x, y, w, h;
    stackcore::detect_object_bounds(img, 4, x, y, w, h);
    MT_CHECK_EQ(x, 26);
    MT_CHECK_EQ(y, 16);
    MT_CHECK_EQ(w, 30 + 8);
    MT_CHECK_EQ(h, 25 + 8);
}

MT_TEST(finishing_デリンギングは縁の外の暗い輪を抑える) {
    const ChannelOffsets none;
    FrameBuffer img(96, 96, 1);
    const FrameBuffer disk = dispersed_planet(96, 96, none);
    for (int y = 0; y < 96; ++y) {
        for (int x = 0; x < 96; ++x) img.row(0, y)[x] = disk.row(1, y)[x];
    }
    FinishingPipeline pipeline;
    pipeline.set_input(std::make_shared<FrameBuffer>(std::move(img)));
    FinishingSettings s;
    s.wavelet.assign(6, stackcore::WaveletLayerParams());
    s.wavelet[0].sharpen = 6.0;
    s.wavelet[1].sharpen = 4.0;
    s.wavelet[2].sharpen = 2.0;
    FrameBuffer ringing, clean;
    pipeline.render(s, ringing);
    s.dering = 1.0;
    pipeline.render(s, clean);
    // 背景（0.02）より暗く沈んだ量を比べる。
    double under_ringing = 0.0, under_clean = 0.0;
    for (int y = 0; y < 96; ++y) {
        for (int x = 0; x < 96; ++x) {
            under_ringing += std::max(0.0, 0.02 - ringing.row(0, y)[x]);
            under_clean += std::max(0.0, 0.02 - clean.row(0, y)[x]);
        }
    }
    MT_CHECK(under_ringing > 0.01);
    MT_CHECK(under_clean < under_ringing * 0.2);
}

MT_TEST(finishing_処理系は変化なしなら入力と同じで_工程の順序を守る) {
    ChannelOffsets shift;
    shift.red_dx = 1.0;
    const FrameBuffer img = dispersed_planet(64, 64, shift);
    auto input = std::make_shared<FrameBuffer>(64, 64, 3);
    for (int c = 0; c < 3; ++c) {
        for (int y = 0; y < 64; ++y) std::memcpy(input->row(c, y), img.row(c, y), sizeof(float) * 64);
    }
    FinishingPipeline pipeline;
    pipeline.set_input(input);
    FinishingSettings s;
    s.wavelet.assign(6, stackcore::WaveletLayerParams());
    MT_CHECK(s.identity());
    FrameBuffer out;
    pipeline.render(s, out);
    MT_CHECK(same_bytes(out, *input));

    // 手で順に掛けた結果と一致する（チャンネル → ウェーブレット → 色 → 形）。
    s.channels.red_dx = 1.0;
    s.wavelet[1].sharpen = 3.0;
    s.color.saturation = 1.4;
    s.geometry.rotate_quarter_turns = 2;
    pipeline.render(s, out);

    FrameBuffer aligned, sharpened, colored, expected;
    stackcore::shift_channels(*input, s.channels, aligned);
    stackcore::WaveletSharpener w;
    w.analyze(aligned, 6);
    w.synthesize(s.wavelet, sharpened);
    stackcore::apply_color(sharpened, s.color, colored);
    stackcore::apply_geometry(colored, s.geometry, expected);
    MT_CHECK(same_bytes(out, expected));

    // 同じ設定なら何度描いても同じ（キャッシュの取り違えがない）。
    FrameBuffer again;
    pipeline.render(s, again);
    MT_CHECK(same_bytes(out, again));
    s.channels.red_dx = 0.0;
    pipeline.render(s, again);
    MT_CHECK(!same_bytes(out, again));
}

MT_TEST(finishing_画面上の切り抜き枠を回転反転の前の座標に戻せる) {
    // 画素ごとに値の違う画像（どの画素がどこへ行ったか分かる）。
    FrameBuffer src(37, 23, 2);
    for (int c = 0; c < 2; ++c) {
        for (int y = 0; y < src.height(); ++y) {
            for (int x = 0; x < src.width(); ++x) src.row(c, y)[x] = static_cast<float>(c * 10000 + y * 100 + x);
        }
    }
    for (int turns = 0; turns < 4; ++turns) {
        for (int flip = 0; flip < 4; ++flip) {
            Geometry g;
            g.rotate_quarter_turns = turns;
            g.flip_horizontal = (flip & 1) != 0;
            g.flip_vertical = (flip & 2) != 0;
            FrameBuffer shown;
            apply_geometry(src, g, shown);
            // 画面で描いた枠（見た目の座標）。
            const int x = 3, y = 5, w = std::min(11, shown.width() - 3), h = std::min(9, shown.height() - 5);
            FrameBuffer expected;
            crop_frame(shown, x, y, w, h, expected);
            // 入力の座標に戻して先に切り抜き、あとから同じ向きにする。
            int ix, iy, iw, ih;
            geometry_output_rect_to_input(g, src.width(), src.height(), x, y, w, h, ix, iy, iw, ih);
            FrameBuffer cut, result;
            crop_frame(src, ix, iy, iw, ih, cut);
            apply_geometry(cut, g, result);
            MT_CHECK_EQ(result.width(), expected.width());
            MT_CHECK_EQ(result.height(), expected.height());
            for (int c = 0; c < 2; ++c) {
                for (int yy = 0; yy < expected.height(); ++yy) {
                    for (int xx = 0; xx < expected.width(); ++xx) {
                        MT_CHECK_EQ(result.row(c, yy)[xx], expected.row(c, yy)[xx]);
                    }
                }
            }
        }
    }
}

MT_TEST(finishing_レベル補正はチャンネル別のあと全体を掛ける) {
    FrameBuffer src(4, 1, 3);
    const float v[4] = {0.0f, 0.2f, 0.5f, 0.9f};
    for (int c = 0; c < 3; ++c) {
        for (int x = 0; x < 4; ++x) src.row(c, 0)[x] = v[x];
    }
    stackcore::LevelsSettings l;
    MT_CHECK(l.identity());
    l.channel[0].white = 0.5;   // R だけ白を下げる
    l.master.black = 0.1;
    l.master.gamma = 2.0;       // 中間調を明るく
    FrameBuffer out;
    stackcore::apply_levels(src, l, out);
    const auto lv = [](double x, double b, double w, double g) {
        double t = (x - b) / (w - b);
        t = std::min(1.0, std::max(0.0, t));
        return std::pow(t, 1.0 / g);
    };
    for (int x = 0; x < 4; ++x) {
        MT_CHECK_NEAR(out.row(0, 0)[x], lv(lv(v[x], 0.0, 0.5, 1.0), 0.1, 1.0, 2.0), 1e-6);
        MT_CHECK_NEAR(out.row(1, 0)[x], lv(v[x], 0.1, 1.0, 2.0), 1e-6);
    }
    // その場で書き換えても同じ。
    stackcore::apply_levels(src, l, src);
    MT_CHECK_NEAR(src.row(0, 0)[2], out.row(0, 0)[2], 0.0);
    // 仕上げの処理系でも同じ経路を通る。
    FinishingSettings s;
    s.levels = l;
    MT_CHECK(!s.identity());
}

MT_TEST(finishing_レベル補正に入る画像のヒストグラム) {
    FrameBuffer f(10, 3, 3);
    for (int c = 0; c < 3; ++c) {
        for (int y = 0; y < 3; ++y) {
            for (int x = 0; x < 10; ++x) f.row(c, y)[x] = c == 0 ? 1.5f : (c == 1 ? -0.2f : 0.5f);
        }
    }
    stackcore::LevelsHistogram h;
    stackcore::compute_levels_histogram(f, h);
    MT_CHECK_EQ(h.channels, 3);
    MT_CHECK_EQ(static_cast<int>(h.counts[1][255]), 30);  // 1を超える値は右端
    MT_CHECK_EQ(static_cast<int>(h.counts[2][0]), 30);    // 負の値は左端
    MT_CHECK_EQ(static_cast<int>(h.counts[3][128]), 30);
    std::uint64_t total = 0;
    for (std::uint32_t n : h.counts[0]) total += n;
    MT_CHECK_EQ(static_cast<int>(total), 90);
    // 処理系は、レベル補正を変えただけでは同じヒストグラムを返す。
    auto input = std::make_shared<FrameBuffer>(10, 3, 3);
    for (int c = 0; c < 3; ++c) for (int y = 0; y < 3; ++y) for (int x = 0; x < 10; ++x) input->row(c, y)[x] = 0.1f * x;
    FinishingPipeline p;
    p.set_input(input, 2);
    FinishingSettings s;
    stackcore::LevelsHistogram a, b;
    FrameBuffer out;
    p.render(s, out, &a);
    s.levels.master.white = 0.5;
    p.render(s, out, &b);
    MT_CHECK(a.counts[0] == b.counts[0]);
    MT_CHECK_NEAR(out.row(0, 0)[3], 0.6f, 1e-6);
}

namespace {

// 暗い空（下駄つき）に浮かぶ円盤。object は円盤の色、pedestal は R・G・B 別の下駄。
FrameBuffer disk_on_sky(int w, int h, double radius, const double object[3], const double pedestal[3]) {
    FrameBuffer f(w, h, 3);
    unsigned seed = 9001u;
    for (int c = 0; c < 3; ++c) {
        for (int y = 0; y < h; ++y) {
            float* row = f.row(c, y);
            for (int x = 0; x < w; ++x) {
                const double r = std::hypot(x - w / 2.0, y - h / 2.0);
                // 円盤の中は中心から縁へなだらかに暗くなる（一様だと暗部と区別できない画像にならない）。
                const double disk = r < radius ? 1.0 - 0.4 * r / radius : 0.0;
                seed = seed * 1664525u + 1013904223u;
                const double noise = 0.0004 * (static_cast<double>((seed >> 8) & 0xFFFF) / 65535.0 - 0.5);
                row[x] = static_cast<float>(pedestal[c] + object[c] * disk + noise);
            }
        }
    }
    f.invalidate_luma();
    return f;
}

}  // namespace

MT_TEST(finishing_背景の空の明るさをチャンネルごとに測る) {
    const double object[3] = {0.30, 0.40, 0.20};
    const double pedestal[3] = {0.016, 0.015, 0.022};
    const FrameBuffer image = disk_on_sky(240, 160, 40.0, object, pedestal);
    const stackcore::SkyBackground sky = stackcore::estimate_sky_background(image);
    MT_CHECK(sky.found);
    for (int c = 0; c < 3; ++c) MT_CHECK_NEAR(sky.channel[c], pedestal[c], 0.0003);

    // 広い画面の小さな惑星（明るい部分が全体の0.2%しかない）でも、背景と認める。
    const FrameBuffer small = disk_on_sky(600, 300, 11.0, object, pedestal);
    MT_CHECK(stackcore::estimate_sky_background(small).found);
}

MT_TEST(finishing_空が写っていない画像や黒く切ってある画像では背景を認めない) {
    // 画面いっぱいに模様がある（月面の内側）。暗部は影であって背景ではない。
    FrameBuffer full(200, 150, 1);
    for (int y = 0; y < 150; ++y) {
        for (int x = 0; x < 200; ++x) {
            full.row(0, y)[x] = static_cast<float>(0.3 + 0.2 * std::sin(x * 0.11) * std::cos(y * 0.07));
        }
    }
    full.invalidate_luma();
    MT_CHECK(!stackcore::estimate_sky_background(full).found);

    // 背景がすでに0（黒く切ってある）。0 の画素は数えないので、背景は見つからない。
    const double object[3] = {0.30, 0.40, 0.20};
    const double none[3] = {0.0, 0.0, 0.0};
    FrameBuffer clipped = disk_on_sky(240, 160, 40.0, object, none);
    for (int c = 0; c < 3; ++c) {
        for (int y = 0; y < 160; ++y) {
            for (int x = 0; x < 240; ++x) {
                if (std::hypot(x - 120.0, y - 80.0) >= 40.0) clipped.row(c, y)[x] = 0.0f;
            }
        }
    }
    clipped.invalidate_luma();
    MT_CHECK(!stackcore::estimate_sky_background(clipped).found);
}

MT_TEST(finishing_背景は色の調整より前に引かれホワイトバランスを狂わせない) {
    // 対象は灰色（R=G=B）だが、下駄は青が高い。引かずにホワイトバランスを取ると、
    // 下駄の色に引きずられて青を下げてしまう（実写の土星で B 0.91 と出た。引いた後は 1.21）。
    const double object[3] = {0.08, 0.08, 0.08};
    const double pedestal[3] = {0.016, 0.015, 0.022};
    auto image = std::make_shared<FrameBuffer>(disk_on_sky(240, 160, 40.0, object, pedestal));

    double biased[3];
    stackcore::estimate_white_balance(*image, biased);
    MT_CHECK(biased[2] < 0.95);  // 引かないと青を下げる

    const stackcore::SkyBackground sky = stackcore::estimate_sky_background(*image);
    FrameBuffer subtracted;
    stackcore::subtract_background(*image, sky.channel, subtracted);
    double gains[3];
    stackcore::estimate_white_balance(subtracted, gains);
    MT_CHECK_NEAR(gains[0], 1.0, 0.01);
    MT_CHECK_NEAR(gains[2], 1.0, 0.01);

    // 処理系を通すと、空は0、円盤の中心は下駄ぶんだけ下がり、そのあとで倍率が掛かる。
    FinishingPipeline pipeline;
    pipeline.set_input(image);
    FinishingSettings settings;
    for (int c = 0; c < 3; ++c) settings.background[c] = sky.channel[c];
    settings.color.gain[0] = 2.0;
    MT_CHECK(!settings.identity());
    FrameBuffer out;
    pipeline.render(settings, out);
    MT_CHECK(out.row(0, 5)[5] < 0.001f);
    MT_CHECK(out.row(2, 5)[5] < 0.001f);
    MT_CHECK_NEAR(out.row(0, 80)[120], 2.0 * 0.08, 0.002);  // (下駄を引いた 0.08) × 2
    MT_CHECK_NEAR(out.row(1, 80)[120], 0.08, 0.002);
}
