#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "microtest.hpp"
#include "stackcore/drizzle.hpp"
#include "stackcore/frame_buffer.hpp"
#include "stackcore/resample.hpp"

using stackcore::Drizzle;
using stackcore::DrizzleStats;
using stackcore::FrameBuffer;

namespace {

struct Lcg {
    std::uint32_t s;
    explicit Lcg(std::uint32_t seed) : s(seed) {}
    double next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<double>((s >> 8) & 0xFFFF) / 65535.0;
    }
};

// 高解像度の原本を連続関数として定義する。
// ガウス斑点の重ね合わせ（非周期・帯域制限）。
struct Scene {
    struct Blob {
        double x, y, a;
    };
    std::vector<Blob> blobs;
    double sigma;

    Scene(double extent, double sig, std::uint32_t seed) : sigma(sig) {
        Lcg rng(seed);
        const int n = static_cast<int>(extent * extent / 220.0);
        for (int i = 0; i < n; ++i) {
            Blob b;
            b.x = rng.next() * extent;
            b.y = rng.next() * extent;
            b.a = 0.3 + 0.7 * rng.next();
            blobs.push_back(b);
        }
    }

    double at(double x, double y) const {
        const double inv = 1.0 / (2.0 * sigma * sigma);
        const double cut = 25.0 * sigma * sigma;
        double v = 0.0;
        for (std::size_t i = 0; i < blobs.size(); ++i) {
            const double dx = x - blobs[i].x, dy = y - blobs[i].y;
            const double r2 = dx * dx + dy * dy;
            if (r2 > cut) continue;
            v += blobs[i].a * std::exp(-r2 * inv);
        }
        return 0.15 + 0.25 * v;
    }
};

// アンダーサンプリングされたフレームを作る。
//
// 低解像度の画素は、原本の 1/scale 四方の面積を積分したもの。
// 「縮小」を面積平均で行うことで、実際のセンサーが取る像に近づける。
// これをやらずに点サンプリングすると、エイリアスが本来より強く出て
// Drizzleが有利になりすぎる。
FrameBuffer sample_low_res(const Scene& scene, int lo_size, double scale, double dx, double dy,
                           double noise, std::uint32_t seed) {
    FrameBuffer fb(lo_size, lo_size, 1);
    Lcg rng(seed);
    const int sub = 4;  // 1画素あたり4x4で積分する
    for (int y = 0; y < lo_size; ++y) {
        float* row = fb.row(0, y);
        for (int x = 0; x < lo_size; ++x) {
            // この低解像度画素が覆う高解像度側の範囲。
            const double base_x = (static_cast<double>(x) + dx) * scale;
            const double base_y = (static_cast<double>(y) + dy) * scale;
            double acc = 0.0;
            for (int sy = 0; sy < sub; ++sy) {
                for (int sx = 0; sx < sub; ++sx) {
                    acc += scene.at(base_x + (sx + 0.5) / sub * scale,
                                    base_y + (sy + 0.5) / sub * scale);
                }
            }
            double v = acc / (sub * sub) + noise * (rng.next() - 0.5);
            row[x] = static_cast<float>(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
        }
    }
    fb.invalidate_luma();
    return fb;
}


// 明るさ・コントラストの違いを吸収したうえでのPSNR。
// 平行移動も少し許す（Drizzleと単純拡大では基準点が半画素ずれる）。
double best_psnr(const FrameBuffer& img, const FrameBuffer& truth, int margin, int search) {
    const int w = img.width(), h = img.height();
    double best = -1.0;
    for (int dy = -search; dy <= search; ++dy) {
        for (int dx = -search; dx <= search; ++dx) {
            double sa = 0, sb = 0, saa = 0, sab = 0;
            int n = 0;
            for (int y = margin; y < h - margin; ++y) {
                for (int x = margin; x < w - margin; ++x) {
                    const int sx = x + dx, sy = y + dy;
                    if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
                    const double a = img.row(0, sy)[sx];
                    const double b = truth.row(0, y)[x];
                    sa += a;
                    sb += b;
                    saa += a * a;
                    sab += a * b;
                    ++n;
                }
            }
            if (n < 16) continue;
            const double denom = n * saa - sa * sa;
            if (std::fabs(denom) < 1e-12) continue;
            const double k = (n * sab - sa * sb) / denom;
            const double c = (sb - k * sa) / n;

            double mse = 0.0;
            for (int y = margin; y < h - margin; ++y) {
                for (int x = margin; x < w - margin; ++x) {
                    const int sx = x + dx, sy = y + dy;
                    if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
                    const double e = k * img.row(0, sy)[sx] + c - truth.row(0, y)[x];
                    mse += e * e;
                }
            }
            mse /= n;
            if (mse <= 0.0) return 200.0;
            const double psnr = 10.0 * std::log10(1.0 / mse);
            if (psnr > best) best = psnr;
        }
    }
    return best;
}

// 単純な2倍拡大（Lanczos3）。Drizzleの比較対象。
FrameBuffer upscale_lanczos(const FrameBuffer& src, double scale) {
    const int ow = static_cast<int>(std::lround(src.width() * scale));
    const int oh = static_cast<int>(std::lround(src.height() * scale));
    FrameBuffer out(ow, oh, 1);
    std::vector<float> row(static_cast<std::size_t>(ow));
    for (int y = 0; y < oh; ++y) {
        // 出力画素の中心 (y+0.5)/scale - 0.5 が入力座標。
        const double sy = (y + 0.5) / scale - 0.5;
        for (int x = 0; x < ow; ++x) {
            const double sx = (x + 0.5) / scale - 0.5;
            float value = 0.0f;
            stackcore::resample_lanczos3(src.plane(0), src.width(), src.height(), src.stride(),
                                         sx, sy, &value, 1, 1, 1);
            out.row(0, y)[x] = value;
        }
    }
    out.invalidate_luma();
    return out;
}

}  // namespace

MT_TEST(drizzle_一様な入力は一様な出力になる) {
    const int n = 32;
    FrameBuffer f(n, n, 1);
    for (int y = 0; y < n; ++y) {
        float* row = f.row(0, y);
        for (int x = 0; x < n; ++x) row[x] = 0.4f;
    }
    f.invalidate_luma();

    Drizzle d(n, n, 1, 2.0, 0.9);
    // 変位を散らして穴が開かないようにする
    const double shifts[4][2] = {{0.0, 0.0}, {0.5, 0.0}, {0.0, 0.5}, {0.5, 0.5}};
    for (const auto& s : shifts) d.add(f, s[0], s[1], 1.0);

    FrameBuffer out;
    DrizzleStats stats;
    d.finish(out, stats);

    MT_CHECK_EQ(out.width(), 64);
    MT_CHECK_EQ(out.height(), 64);
    MT_CHECK_EQ(static_cast<int>(stats.uncovered_pixels), 0);
    for (int y = 6; y < 58; ++y) {
        for (int x = 6; x < 58; ++x) MT_CHECK_NEAR(out.row(0, y)[x], 0.4f, 1e-5);
    }
}

MT_TEST(drizzle_倍率が出力サイズに反映される) {
    FrameBuffer f(40, 30, 1);
    for (int y = 0; y < 30; ++y) {
        float* row = f.row(0, y);
        for (int x = 0; x < 40; ++x) row[x] = 0.5f;
    }
    f.invalidate_luma();

    for (double scale : {1.0, 1.5, 2.0, 3.0}) {
        Drizzle d(40, 30, 1, scale, 0.9);
        MT_CHECK_EQ(d.out_width(), static_cast<int>(std::lround(40 * scale)));
        MT_CHECK_EQ(d.out_height(), static_cast<int>(std::lround(30 * scale)));
    }
}

MT_TEST(drizzle_輝度正規化の係数が掛かる) {
    const int n = 16;
    FrameBuffer f(n, n, 1);
    for (int y = 0; y < n; ++y) {
        float* row = f.row(0, y);
        for (int x = 0; x < n; ++x) row[x] = 0.25f;
    }
    f.invalidate_luma();

    Drizzle d(n, n, 1, 1.0, 1.0);
    d.add(f, 0.0, 0.0, 2.0);
    FrameBuffer out;
    DrizzleStats stats;
    d.finish(out, stats);
    MT_CHECK_NEAR(out.row(0, 8)[8], 0.5f, 1e-5);
}

MT_TEST(drizzle_pixfracを小さくすると穴が開く) {
    // 1枚しか落とさず pixfrac を小さくすれば、寄与を受けない出力画素が出る。
    // これは不具合ではなくDrizzleの性質であり、UIで説明すべき事柄。
    const int n = 16;
    FrameBuffer f(n, n, 1);
    for (int y = 0; y < n; ++y) {
        float* row = f.row(0, y);
        for (int x = 0; x < n; ++x) row[x] = 0.5f;
    }
    f.invalidate_luma();

    Drizzle d(n, n, 1, 3.0, 0.3);
    d.add(f, 0.0, 0.0, 1.0);
    FrameBuffer out;
    DrizzleStats stats;
    d.finish(out, stats);
    MT_CHECK(stats.uncovered_pixels > 0);
}

MT_TEST(drizzle_不正な引数は例外) {
    FrameBuffer f(8, 8, 1);
    MT_CHECK_THROWS(Drizzle(8, 8, 1, 0.0, 0.9));
    MT_CHECK_THROWS(Drizzle(8, 8, 1, 2.0, 0.0));
    MT_CHECK_THROWS(Drizzle(8, 8, 1, 2.0, 1.5));
    MT_CHECK_THROWS(Drizzle(0, 8, 1, 2.0, 0.9));
}

MT_TEST(drizzle_アンダーサンプリングで単純拡大より原本に近い) {
    // M5の受け入れ条件。
    //
    // 高解像度の原本 → 1/2に縮小（面積平均）＋サブピクセル変位 の群を作り、
    // 2xDrizzle と 単純2x拡大 を原本と比べる。
    //
    // 斑点のσを1.1にして、低解像度側ではアンダーサンプリングになるようにしてある。
    // Drizzleが効くのはこの条件のときだけである（仕様書 §4.9）。
    const int lo = 48;
    const double scale = 2.0;
    const int hi = static_cast<int>(lo * scale);
    const Scene scene(static_cast<double>(hi), 1.1, 2026);

    // フレームごとのサブピクセル変位。低解像度画素の単位で与える。
    // 1/2画素刻みを埋めるように散らす（実際の撮影ではシーイングで自然に散る）。
    const double shifts[8][2] = {{0.00, 0.00}, {0.50, 0.00}, {0.00, 0.50}, {0.50, 0.50},
                                 {0.25, 0.25}, {0.75, 0.25}, {0.25, 0.75}, {0.75, 0.75}};

    Drizzle d(lo, lo, 1, scale, 0.7);
    std::vector<FrameBuffer> frames;
    for (int i = 0; i < 8; ++i) {
        FrameBuffer f =
            sample_low_res(scene, lo, scale, shifts[i][0], shifts[i][1], 0.004, 100u + i);
        d.add(f, shifts[i][0], shifts[i][1], 1.0);
        frames.push_back(std::move(f));
    }

    FrameBuffer drizzled;
    DrizzleStats stats;
    d.finish(drizzled, stats);

    // 比較対象: 変位0のフレームを単純に2x拡大したもの。
    const FrameBuffer upscaled = upscale_lanczos(frames[0], scale);

    // 真値: 原本を出力グリッドで標本化したもの。
    FrameBuffer truth(hi, hi, 1);
    for (int y = 0; y < hi; ++y) {
        float* row = truth.row(0, y);
        for (int x = 0; x < hi; ++x) {
            const double v = scene.at(x + 0.5, y + 0.5);
            row[x] = static_cast<float>(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
        }
    }
    truth.invalidate_luma();

    const double psnr_drizzle = best_psnr(drizzled, truth, 8, 3);
    const double psnr_upscale = best_psnr(upscaled, truth, 8, 3);

    std::printf("           PSNR: 2xDrizzle %.2f dB / 単純2x拡大 %.2f dB (差 %+.2f dB)\n",
                psnr_drizzle, psnr_upscale, psnr_drizzle - psnr_upscale);
    MT_CHECK_EQ(static_cast<int>(stats.uncovered_pixels), 0);
    if (!(psnr_drizzle > psnr_upscale)) {
        microtest::fail("Drizzleが単純拡大より原本に近くない: Drizzle " +
                        microtest::mt_str(psnr_drizzle) + " dB / 拡大 " +
                        microtest::mt_str(psnr_upscale) + " dB");
    }
}

MT_TEST(drizzle_同じ順序なら結果はビット単位で同じ) {
    const int n = 24;
    const Scene scene(48.0, 1.2, 77);
    std::vector<FrameBuffer> frames;
    for (int i = 0; i < 4; ++i) {
        frames.push_back(sample_low_res(scene, n, 2.0, i * 0.25, i * 0.13, 0.005, 200u + i));
    }

    FrameBuffer a, b;
    DrizzleStats sa, sb;
    for (int run = 0; run < 2; ++run) {
        Drizzle d(n, n, 1, 2.0, 0.9);
        for (int i = 0; i < 4; ++i) d.add(frames[static_cast<std::size_t>(i)], i * 0.25, i * 0.13, 1.0);
        d.finish(run == 0 ? a : b, run == 0 ? sa : sb);
    }
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (a.row(0, y)[x] != b.row(0, y)[x]) {
                microtest::fail("同じ順序で結果が一致しない");
                return;
            }
        }
    }
}
