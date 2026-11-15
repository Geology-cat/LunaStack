#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "microtest.hpp"
#include "stackcore/mosaic.hpp"

using stackcore::FrameBuffer;
using stackcore::MosaicReport;
using stackcore::MosaicSettings;

namespace {

// 連続関数として定義した「月面もどき」。格子点の乱数をなめらかにつないだ模様を
// 3つの細かさで重ねる。小数位置・回転した位置でも、補間なしで直接評価できる。
struct Terrain {
    std::uint32_t seed;
    explicit Terrain(std::uint32_t s) : seed(s) {}

    static double hash(std::uint32_t seed, int x, int y) {
        std::uint32_t h = seed ^ (static_cast<std::uint32_t>(x) * 374761393u) ^
                          (static_cast<std::uint32_t>(y) * 668265263u);
        h = (h ^ (h >> 13)) * 1274126177u;
        h ^= h >> 16;
        return static_cast<double>(h & 0xFFFFFF) / static_cast<double>(0xFFFFFF);
    }

    static double smooth(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

    double noise(double x, double y, std::uint32_t layer) const {
        const double fx = std::floor(x), fy = std::floor(y);
        const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
        const double tx = smooth(x - fx), ty = smooth(y - fy);
        const double a = hash(seed + layer, ix, iy), b = hash(seed + layer, ix + 1, iy);
        const double c = hash(seed + layer, ix, iy + 1), d = hash(seed + layer, ix + 1, iy + 1);
        return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
    }

    double at(double x, double y) const {
        const double v = 0.30 + 0.22 * noise(x / 46.0, y / 46.0, 1) + 0.16 * noise(x / 15.0, y / 15.0, 2) +
                         0.10 * noise(x / 6.0, y / 6.0, 3);
        // なだらかな陰り（欠け際に向かって暗くなる）。
        return v * (0.75 + 0.25 * std::sin(x / 310.0 + 0.4));
    }
};

struct PanelSpec {
    double x0, y0;       // パネルの (0,0) が場面のどこか
    int w, h;
    double degrees;      // パネルの中心まわりの回転
    double gain;
};

struct Lcg {
    std::uint32_t s;
    explicit Lcg(std::uint32_t seed) : s(seed) {}
    double next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<double>((s >> 8) & 0xFFFF) / 65535.0;
    }
};

// パネルの画素 (u, v) が場面のどこを見ているか。
void panel_to_scene(const PanelSpec& p, double u, double v, double& x, double& y) {
    const double cx = (p.w - 1) * 0.5, cy = (p.h - 1) * 0.5;
    const double r = p.degrees * M_PI / 180.0;
    const double du = u - cx, dv = v - cy;
    x = p.x0 + cx + std::cos(r) * du - std::sin(r) * dv;
    y = p.y0 + cy + std::sin(r) * du + std::cos(r) * dv;
}

FrameBuffer make_panel(const Terrain& terrain, const PanelSpec& p, int channels, double noise,
                       std::uint32_t seed) {
    FrameBuffer f(p.w, p.h, channels);
    Lcg rng(seed);
    for (int c = 0; c < channels; ++c) {
        for (int v = 0; v < p.h; ++v) {
            float* row = f.row(c, v);
            for (int u = 0; u < p.w; ++u) {
                double x, y;
                panel_to_scene(p, u, v, x, y);
                double value = terrain.at(x, y) * p.gain * (1.0 - 0.1 * c);
                value += noise * (rng.next() - 0.5);
                row[u] = static_cast<float>(value < 0.001 ? 0.001 : (value > 1.0 ? 1.0 : value));
            }
        }
    }
    f.invalidate_luma();
    return f;
}

// モザイクと場面の食い違い（明るさの倍率は最小二乗で合わせる）。
// パネル0は回転なし・整数位置に置いてあるので、モザイクの座標は場面の座標と整数だけずれる。
double mosaic_rms(const FrameBuffer& mosaic, const MosaicReport& report, const Terrain& terrain,
                  const PanelSpec& first, int margin) {
    const double ox = first.x0 - report.panels[0].tx, oy = first.y0 - report.panels[0].ty;
    double saa = 0.0, sab = 0.0;
    for (int y = margin; y < mosaic.height() - margin; ++y) {
        for (int x = margin; x < mosaic.width() - margin; ++x) {
            const double a = mosaic.row(0, y)[x];
            if (!(a > 0.0)) continue;
            const double b = terrain.at(x + ox, y + oy);
            saa += a * a;
            sab += a * b;
        }
    }
    const double k = sab / saa;
    double err = 0.0;
    long long count = 0;
    for (int y = margin; y < mosaic.height() - margin; ++y) {
        for (int x = margin; x < mosaic.width() - margin; ++x) {
            const double a = mosaic.row(0, y)[x];
            if (!(a > 0.0)) continue;
            const double d = k * a - terrain.at(x + ox, y + oy);
            err += d * d;
            ++count;
        }
    }
    return std::sqrt(err / static_cast<double>(count > 0 ? count : 1));
}

std::vector<const FrameBuffer*> pointers(const std::vector<FrameBuffer>& panels) {
    std::vector<const FrameBuffer*> out;
    for (const FrameBuffer& p : panels) out.push_back(&p);
    return out;
}

}  // namespace

MT_TEST(mosaic_2x2のパネルを小数位置と明るさの差つきでつなぐ) {
    const Terrain terrain(2026);
    const std::vector<PanelSpec> specs = {
        {40.0, 30.0, 300, 260, 0.0, 1.00},
        {281.3, 24.6, 300, 260, 0.0, 0.92},
        {33.7, 229.4, 300, 260, 0.0, 1.07},
        {276.2, 236.8, 300, 260, 0.0, 0.97},
    };
    std::vector<FrameBuffer> panels;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        panels.push_back(make_panel(terrain, specs[i], 1, 0.006, 100 + static_cast<std::uint32_t>(i)));
    }
    MosaicReport report;
    const FrameBuffer mosaic =
        stackcore::build_mosaic(pointers(panels), MosaicSettings(), nullptr, report);

    MT_CHECK_EQ(report.placed, 4);
    MT_CHECK(!report.used_rotation);
    // 置いた位置（パネル0からの相対）が真の位置と合う。
    double worst = 0.0;
    for (std::size_t i = 1; i < specs.size(); ++i) {
        const double ex = (report.panels[i].tx - report.panels[0].tx) - (specs[i].x0 - specs[0].x0);
        const double ey = (report.panels[i].ty - report.panels[0].ty) - (specs[i].y0 - specs[0].y0);
        worst = std::max(worst, std::hypot(ex, ey));
    }
    // 明るさの比が戻る（撮ったときの倍率の逆数に比例する）。
    double gain_error = 0.0;
    for (std::size_t i = 1; i < specs.size(); ++i) {
        const double ratio = (report.panels[i].gain * specs[i].gain) / (report.panels[0].gain * specs[0].gain);
        gain_error = std::max(gain_error, std::fabs(ratio - 1.0));
    }
    const double rms = mosaic_rms(mosaic, report, terrain, specs[0], 4);
    std::printf("           %dx%d / 位置の誤差 最大 %.3f px / 明るさの誤差 最大 %.2f%% / 真値とのRMS %.5f / 残差 %.2f px\n",
                mosaic.width(), mosaic.height(), worst, gain_error * 100.0, rms, report.residual_rms);
    MT_CHECK(worst < 0.15);
    MT_CHECK(gain_error < 0.01);
    MT_CHECK(rms < 0.004);
    MT_CHECK(mosaic.width() >= 547 && mosaic.width() <= 550);
    MT_CHECK(mosaic.height() >= 471 && mosaic.height() <= 474);
}

MT_TEST(mosaic_小さな回転があるパネルもつなぐ) {
    // 経緯台の視野回転を想定（パネルごとに少しずつ回る）。
    const Terrain terrain(77);
    const std::vector<PanelSpec> specs = {
        {20.0, 20.0, 320, 280, 0.0, 1.00},
        {262.4, 31.2, 320, 280, 1.3, 1.00},
        {31.5, 240.7, 320, 280, -0.8, 1.00},
        {270.9, 232.1, 320, 280, 2.1, 1.00},
    };
    std::vector<FrameBuffer> panels;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        panels.push_back(make_panel(terrain, specs[i], 1, 0.006, 300 + static_cast<std::uint32_t>(i)));
    }
    MosaicReport report;
    const FrameBuffer mosaic =
        stackcore::build_mosaic(pointers(panels), MosaicSettings(), nullptr, report);
    MT_CHECK_EQ(report.placed, 4);
    MT_CHECK(report.used_rotation);
    double worst = 0.0;
    for (std::size_t i = 1; i < specs.size(); ++i) {
        const double degrees = std::atan2(report.panels[i].b, report.panels[i].a) * 180.0 / M_PI;
        worst = std::max(worst, std::fabs(degrees - specs[i].degrees));
    }
    const double rms = mosaic_rms(mosaic, report, terrain, specs[0], 4);
    std::printf("           回転の誤差 最大 %.3f° / 真値とのRMS %.5f / 残差 %.2f px\n", worst, rms,
                report.residual_rms);
    MT_CHECK(worst < 0.05);
    MT_CHECK(rms < 0.005);

    // 回転を補正しない設定では、平行移動だけで置く。
    MosaicSettings translation_only;
    translation_only.allow_rotation = false;
    MosaicReport r2;
    stackcore::build_mosaic(pointers(panels), translation_only, nullptr, r2);
    MT_CHECK(!r2.used_rotation);
}

MT_TEST(mosaic_重ならないパネルは置かずに報告する) {
    const Terrain terrain(5), other(999);
    const std::vector<PanelSpec> specs = {
        {0.0, 0.0, 280, 240, 0.0, 1.0},
        {215.5, 12.3, 280, 240, 0.0, 1.0},
        {0.0, 0.0, 280, 240, 0.0, 1.0},  // 別の場面
    };
    std::vector<FrameBuffer> panels;
    panels.push_back(make_panel(terrain, specs[0], 1, 0.006, 1));
    panels.push_back(make_panel(terrain, specs[1], 1, 0.006, 2));
    panels.push_back(make_panel(other, specs[2], 1, 0.006, 3));
    MosaicReport report;
    const FrameBuffer mosaic =
        stackcore::build_mosaic(pointers(panels), MosaicSettings(), nullptr, report);
    MT_CHECK_EQ(report.placed, 2);
    MT_CHECK(report.panels[0].placed);
    MT_CHECK(report.panels[1].placed);
    MT_CHECK(!report.panels[2].placed);
    MT_CHECK(mosaic.width() >= 494 && mosaic.width() <= 498);

    // 1組もつながらなければエラーにする。
    std::vector<FrameBuffer> unrelated;
    unrelated.push_back(make_panel(terrain, specs[0], 1, 0.006, 1));
    unrelated.push_back(make_panel(other, specs[2], 1, 0.006, 3));
    bool threw = false;
    try {
        MosaicReport r;
        stackcore::build_mosaic(pointers(unrelated), MosaicSettings(), nullptr, r);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    MT_CHECK(threw);
}

MT_TEST(mosaic_カラーとモノクロが混ざってもつなげて結果はビット単位で再現する) {
    const Terrain terrain(31);
    const std::vector<PanelSpec> specs = {
        {10.0, 10.0, 260, 220, 0.0, 1.0},
        {206.6, 17.4, 260, 220, 0.0, 1.0},
    };
    std::vector<FrameBuffer> panels;
    panels.push_back(make_panel(terrain, specs[0], 3, 0.006, 11));
    panels.push_back(make_panel(terrain, specs[1], 1, 0.006, 12));
    MosaicReport r1, r2;
    MosaicSettings settings;
    settings.match_brightness = false;
    const FrameBuffer a = stackcore::build_mosaic(pointers(panels), settings, nullptr, r1);
    const FrameBuffer b = stackcore::build_mosaic(pointers(panels), settings, nullptr, r2);
    MT_CHECK_EQ(a.channels(), 3);
    MT_CHECK_EQ(r1.placed, 2);
    bool same = a.width() == b.width() && a.height() == b.height();
    for (int c = 0; same && c < a.channels(); ++c) {
        for (int y = 0; same && y < a.height(); ++y) {
            same = std::memcmp(a.row(c, y), b.row(c, y), sizeof(float) * static_cast<std::size_t>(a.width())) == 0;
        }
    }
    MT_CHECK(same);
}

MT_TEST(mosaic_中断できる) {
    const Terrain terrain(8);
    const std::vector<PanelSpec> specs = {
        {0.0, 0.0, 260, 220, 0.0, 1.0},
        {200.0, 10.0, 260, 220, 0.0, 1.0},
    };
    std::vector<FrameBuffer> panels;
    panels.push_back(make_panel(terrain, specs[0], 1, 0.0, 1));
    panels.push_back(make_panel(terrain, specs[1], 1, 0.0, 2));
    bool cancelled = false;
    try {
        MosaicReport r;
        stackcore::build_mosaic(pointers(panels), MosaicSettings(),
                                [](const char*, int, int) { return false; }, r);
    } catch (const stackcore::Cancelled&) {
        cancelled = true;
    }
    MT_CHECK(cancelled);
}

namespace {

// 暗い空に浮かぶ月面もどき。画素値 = カメラの下駄 + 透明度 × 月の光。
// 透明度（gain）で変わるのは月の光だけで、下駄は変わらない。
FrameBuffer make_sky_panel(const Terrain& terrain, double x0, double y0, int w, int h, double gain,
                           double pedestal, std::uint32_t seed) {
    FrameBuffer f(w, h, 1);
    Lcg rng(seed);
    for (int v = 0; v < h; ++v) {
        float* row = f.row(0, v);
        for (int u = 0; u < w; ++u) {
            const double x = x0 + u, y = y0 + v;
            // 中心 (260, 250)・半径 150 の円盤。縁は 3px でなだらかに落とす。
            const double r = std::hypot(x - 260.0, y - 250.0);
            const double disk = r < 148.5 ? 1.0 : (r > 151.5 ? 0.0 : (151.5 - r) / 3.0);
            double value = pedestal + gain * disk * terrain.at(x, y);
            value += 0.0006 * (rng.next() - 0.5);
            row[u] = static_cast<float>(value);
        }
    }
    f.invalidate_luma();
    return f;
}

double region_mean(const FrameBuffer& f, int x0, int y0, int w, int h) {
    double sum = 0.0;
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) sum += f.row(0, y)[x];
    }
    return sum / (static_cast<double>(w) * h);
}

}  // namespace

MT_TEST(mosaic_明るさの倍率は空より上にだけ掛け空の明るさをそろえる) {
    // 実写（ASI715MC、下駄 0.016、パネル間の明るさの差 15%）で、下駄ごと倍率を掛けていたため
    // 空の明るさがパネルごとに変わり、黒レベルを詰めると段差と斜めの筋が見えた。
    const Terrain terrain(77);
    const double pedestal = 0.02;
    const FrameBuffer top = make_sky_panel(terrain, 0.0, 0.0, 520, 300, 1.00, pedestal, 1);
    const FrameBuffer bottom = make_sky_panel(terrain, 0.0, 200.0, 520, 300, 0.82, pedestal, 2);

    MosaicReport report;
    const FrameBuffer mosaic =
        stackcore::build_mosaic({&top, &bottom}, MosaicSettings(), nullptr, report);
    MT_CHECK_EQ(report.placed, 2);
    MT_CHECK_NEAR(report.background, pedestal, 0.001);
    // g_top × 1.00 = g_bottom × 0.82
    MT_CHECK_NEAR(report.panels[0].gain / report.panels[1].gain, 0.82, 0.01);

    // 上のパネルだけの空・重なりの空・下のパネルだけの空。どれも下駄の明るさのまま。
    const double sky_top = region_mean(mosaic, 440, 20, 60, 60);
    const double sky_overlap = region_mean(mosaic, 440, 220, 60, 60);
    const double sky_bottom = region_mean(mosaic, 440, 420, 60, 60);
    std::printf("           空の明るさ: 上 %.5f / 重なり %.5f / 下 %.5f（下駄 %.3f）\n", sky_top,
                sky_overlap, sky_bottom, pedestal);
    MT_CHECK_NEAR(sky_top, pedestal, 0.0003);
    MT_CHECK_NEAR(sky_overlap, pedestal, 0.0003);
    MT_CHECK_NEAR(sky_bottom, pedestal, 0.0003);
}
