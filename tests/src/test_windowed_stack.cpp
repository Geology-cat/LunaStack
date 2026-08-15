#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "microtest.hpp"
#include "stackcore/ap_placer.hpp"
#include "stackcore/frame_buffer.hpp"
#include "stackcore/local_aligner.hpp"
#include "stackcore/windowed_stacker.hpp"

using stackcore::AlignmentPoint;
using stackcore::FrameBuffer;
using stackcore::LocalAlignSettings;
using stackcore::LocalMatch;
using stackcore::WindowedStacker;
using stackcore::WindowedStackStats;

namespace {

struct Lcg {
    std::uint32_t s;
    explicit Lcg(std::uint32_t seed) : s(seed) {}
    double next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<double>((s >> 8) & 0xFFFF) / 65535.0;
    }
};

FrameBuffer constant_frame(int w, int h, int c, float v) {
    FrameBuffer fb(w, h, c);
    for (int ch = 0; ch < c; ++ch) {
        for (int y = 0; y < h; ++y) {
            float* row = fb.row(ch, y);
            for (int x = 0; x < w; ++x) row[x] = v;
        }
    }
    fb.invalidate_luma();
    return fb;
}

// 非周期の連続テクスチャ。ガウス斑点の重ね合わせ。
struct Texture {
    struct Blob {
        double x, y, a;
    };
    std::vector<Blob> list;
    double sigma = 2.2;

    explicit Texture(int extent, std::uint32_t seed) {
        Lcg rng(seed);
        const int n = extent * extent / 90;
        for (int i = 0; i < n; ++i) {
            Blob b;
            b.x = rng.next() * extent;
            b.y = rng.next() * extent;
            b.a = 0.3 + 0.7 * rng.next();
            list.push_back(b);
        }
    }

    double at(double x, double y) const {
        const double inv = 1.0 / (2.0 * sigma * sigma);
        double v = 0.0;
        for (std::size_t i = 0; i < list.size(); ++i) {
            const double dx = x - list[i].x, dy = y - list[i].y;
            const double r2 = dx * dx + dy * dy;
            if (r2 > 25.0 * sigma * sigma) continue;
            v += list[i].a * std::exp(-r2 * inv);
        }
        return 0.25 + 0.28 * v;
    }
};

FrameBuffer sample_texture(const Texture& t, int size) {
    FrameBuffer fb(size, size, 1);
    for (int y = 0; y < size; ++y) {
        float* row = fb.row(0, y);
        for (int x = 0; x < size; ++x) {
            double v = t.at(x, y);
            row[x] = static_cast<float>(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
        }
    }
    fb.invalidate_luma();
    return fb;
}

// 出力に格子ピッチの周期成分があるかを調べる。
// 目視では1LSB規模のさざ波は見つからないので、周波数で直接測る。
// 行方向の平均プロファイルに対して、周期 pitch の成分の振幅を返す。
double grid_ripple_amplitude(const FrameBuffer& img, int pitch, int margin) {
    const int w = img.width(), h = img.height();
    std::vector<double> profile(static_cast<std::size_t>(w), 0.0);
    for (int x = margin; x < w - margin; ++x) {
        double s = 0.0;
        int n = 0;
        for (int y = margin; y < h - margin; ++y) {
            s += img.row(0, y)[x];
            ++n;
        }
        profile[static_cast<std::size_t>(x)] = n > 0 ? s / n : 0.0;
    }

    // 周期 pitch の成分を相関で取り出す。
    double re = 0.0, im = 0.0, count = 0.0;
    for (int x = margin; x < w - margin; ++x) {
        const double phase = 2.0 * M_PI * x / pitch;
        re += profile[static_cast<std::size_t>(x)] * std::cos(phase);
        im += profile[static_cast<std::size_t>(x)] * std::sin(phase);
        count += 1.0;
    }
    if (count <= 0.0) return 0.0;
    return 2.0 * std::sqrt(re * re + im * im) / count;
}

}  // namespace

MT_TEST(wstack_一様な入力は一様な出力になる) {
    // 窓の重み付けと S/W の割り算が正しければ、値はそのまま保たれる。
    const int n = 128, ap = 32;
    WindowedStacker st(n, n, 1, ap);
    const FrameBuffer f = constant_frame(n, n, 1, 0.4f);

    for (int cy = ap / 2; cy + ap / 2 <= n; cy += ap / 2) {
        for (int cx = ap / 2; cx + ap / 2 <= n; cx += ap / 2) {
            st.begin_ap(cx, cy);
            st.add_frame(f, 0.0, 0.0, 1.0);
            st.end_ap();
        }
    }

    FrameBuffer out;
    WindowedStackStats stats;
    st.finish(out, stats, &f);

    for (int y = ap; y < n - ap; ++y) {
        for (int x = ap; x < n - ap; ++x) {
            MT_CHECK_NEAR(out.row(0, y)[x], 0.4f, 1e-5);
        }
    }
}

MT_TEST(wstack_Drizzleでも一様な入力は一様な出力になる) {
    // **これが最初に通すべきテスト。**
    // Drizzleを入れると重みが2種類になる（Hann窓 と 落とし込みの面積）。
    // その積を取り違えると S/W の正規化が狂い、AP中心に向かって
    // 明るさが偏る。一様入力なら必ず一様出力になるはずなので、
    // ここが崩れていれば実データを見る前に分かる。
    const int n = 128, ap = 32;
    const double scale = 2.0;
    WindowedStacker st(n, n, 1, ap, scale, 0.9);
    MT_CHECK_EQ(st.out_width(), 256);
    MT_CHECK_EQ(st.out_height(), 256);

    const FrameBuffer f = constant_frame(n, n, 1, 0.4f);
    // 変位を小数で散らす（Drizzleが意味を持つ条件）
    const double shifts[4][2] = {{0.0, 0.0}, {0.5, 0.0}, {0.0, 0.5}, {0.5, 0.5}};
    for (int cy = ap / 2; cy + ap / 2 <= n; cy += ap / 2) {
        for (int cx = ap / 2; cx + ap / 2 <= n; cx += ap / 2) {
            st.begin_ap(cx, cy);
            for (const auto& sh : shifts) st.add_frame(f, sh[0], sh[1], 1.0);
            st.end_ap();
        }
    }

    FrameBuffer out;
    WindowedStackStats stats;
    st.finish(out, stats, &f);

    double worst = 0.0;
    for (int y = ap * 2; y < 256 - ap * 2; ++y) {
        for (int x = ap * 2; x < 256 - ap * 2; ++x) {
            const double d = std::fabs(out.row(0, y)[x] - 0.4);
            if (d > worst) worst = d;
        }
    }
    std::printf("           Drizzle 2x の一様性: 最大ずれ %.2e\n", worst);
    if (!(worst < 1e-4)) {
        microtest::fail("Drizzleで一様入力が一様に出ない: 最大ずれ " +
                        microtest::mt_str(worst));
    }
}

MT_TEST(wstack_Drizzleは等倍のときビット単位で従来と同じ) {
    // 倍率1.0では従来のLanczos経路を通すことの確認。
    // Drizzle経路に一本化するとM2までの出力が変わってしまう。
    const int n = 96, ap = 32;
    const Texture tex(n, 4);
    const FrameBuffer f = sample_texture(tex, n);

    FrameBuffer a, b;
    WindowedStackStats sa, sb;
    {
        WindowedStacker st(n, n, 1, ap);  // 既定（scale=1.0）
        st.begin_ap(48, 48);
        for (int k = 0; k < 3; ++k) st.add_frame(f, k * 0.31, -k * 0.17, 1.0);
        st.end_ap();
        st.finish(a, sa, nullptr);
    }
    {
        WindowedStacker st(n, n, 1, ap, 1.0, 0.9);  // 明示的に1.0
        st.begin_ap(48, 48);
        for (int k = 0; k < 3; ++k) st.add_frame(f, k * 0.31, -k * 0.17, 1.0);
        st.end_ap();
        st.finish(b, sb, nullptr);
    }
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            if (a.row(0, y)[x] != b.row(0, y)[x]) {
                microtest::fail("倍率1.0の指定で結果が変わった");
                return;
            }
        }
    }
}

MT_TEST(wstack_Drizzleの未被覆領域は拡大した代替画像で埋まる) {
    const int n = 96, ap = 32;
    WindowedStacker st(n, n, 1, ap, 2.0, 0.9);
    const FrameBuffer f = constant_frame(n, n, 1, 0.5f);
    const FrameBuffer fallback = constant_frame(n, n, 1, 0.125f);

    st.begin_ap(48, 48);
    st.add_frame(f, 0.0, 0.0, 1.0);
    st.end_ap();

    FrameBuffer out;
    WindowedStackStats stats;
    st.finish(out, stats, &fallback);

    MT_CHECK_EQ(out.width(), 192);
    MT_CHECK(stats.uncovered_pixels > 0);
    // 端は代替画像（拡大されたもの）で埋まる
    MT_CHECK_NEAR(out.row(0, 0)[0], 0.125f, 1e-4);
    // AP中心はスタック結果
    MT_CHECK_NEAR(out.row(0, 96)[96], 0.5f, 1e-4);
}

MT_TEST(wstack_APが掛からない領域は代替画像で埋まる) {
    const int n = 96, ap = 32;
    WindowedStacker st(n, n, 1, ap);
    const FrameBuffer f = constant_frame(n, n, 1, 0.5f);
    const FrameBuffer fallback = constant_frame(n, n, 1, 0.125f);

    // APを1つだけ、中央に置く。
    st.begin_ap(48, 48);
    st.add_frame(f, 0.0, 0.0, 1.0);
    st.end_ap();

    FrameBuffer out;
    WindowedStackStats stats;
    st.finish(out, stats, &fallback);

    MT_CHECK(stats.uncovered_pixels > 0);
    MT_CHECK_NEAR(out.row(0, 0)[0], 0.125f, 1e-6);   // 端は代替画像
    MT_CHECK_NEAR(out.row(0, 48)[48], 0.5f, 1e-5);   // AP中心はスタック結果
}

MT_TEST(wstack_代替画像がなければ未被覆領域は0になる) {
    const int n = 96, ap = 32;
    WindowedStacker st(n, n, 1, ap);
    const FrameBuffer f = constant_frame(n, n, 1, 0.5f);

    st.begin_ap(48, 48);
    st.add_frame(f, 0.0, 0.0, 1.0);
    st.end_ap();

    FrameBuffer out;
    WindowedStackStats stats;
    st.finish(out, stats, nullptr);
    MT_CHECK_NEAR(out.row(0, 0)[0], 0.0f, 1e-9);
}

MT_TEST(wstack_輝度正規化の係数が掛かる) {
    const int n = 64, ap = 32;
    WindowedStacker st(n, n, 1, ap);
    const FrameBuffer f = constant_frame(n, n, 1, 0.2f);

    st.begin_ap(32, 32);
    st.add_frame(f, 0.0, 0.0, 2.0);
    st.end_ap();

    FrameBuffer out;
    WindowedStackStats stats;
    st.finish(out, stats, nullptr);
    MT_CHECK_NEAR(out.row(0, 32)[32], 0.4f, 1e-5);
}

MT_TEST(wstack_品質重み付き平均は指定した重みで平均する) {
    const int n = 64, ap = 32;
    WindowedStacker st(n, n, 1, ap, 1.0, 0.9, stackcore::StackMode::QualityWeighted);
    const FrameBuffer dark = constant_frame(n, n, 1, 0.2f);
    const FrameBuffer bright = constant_frame(n, n, 1, 0.8f);

    st.begin_ap(32, 32);
    st.add_frame(dark, 0.0, 0.0, 1.0, 1.0);
    st.add_frame(bright, 0.0, 0.0, 1.0, 3.0);
    st.end_ap();

    FrameBuffer out;
    WindowedStackStats stats;
    st.finish(out, stats, nullptr);
    MT_CHECK_NEAR(out.row(0, 32)[32], 0.65f, 1e-5);
}

MT_TEST(wstack_シグマクリップは単発の外れ値を除く) {
    const int n = 64, ap = 32;
    WindowedStacker st(n, n, 1, ap, 1.0, 0.9, stackcore::StackMode::SigmaClip, 2.0);
    const FrameBuffer normal = constant_frame(n, n, 1, 0.2f);
    const FrameBuffer outlier = constant_frame(n, n, 1, 1.0f);

    st.begin_ap(32, 32);
    st.add_frame(normal, 0.0, 0.0, 1.0);
    st.add_frame(normal, 0.0, 0.0, 1.0);
    st.add_frame(normal, 0.0, 0.0, 1.0);
    st.add_frame(outlier, 0.0, 0.0, 1.0);
    st.end_ap();

    FrameBuffer out;
    WindowedStackStats stats;
    st.finish(out, stats, nullptr);
    MT_CHECK_NEAR(out.row(0, 32)[32], 0.2f, 1e-5);
}

MT_TEST(wstack_同じ順序なら結果はビット単位で同じ) {
    const int n = 96, ap = 32;
    const Texture tex(n, 4);
    const FrameBuffer f = sample_texture(tex, n);

    FrameBuffer a, b;
    WindowedStackStats sa, sb;
    for (int run = 0; run < 2; ++run) {
        WindowedStacker st(n, n, 1, ap);
        for (int cy = ap / 2; cy + ap / 2 <= n; cy += ap / 2) {
            for (int cx = ap / 2; cx + ap / 2 <= n; cx += ap / 2) {
                st.begin_ap(cx, cy);
                for (int k = 0; k < 5; ++k) st.add_frame(f, k * 0.13, -k * 0.07, 1.0);
                st.end_ap();
            }
        }
        st.finish(run == 0 ? a : b, run == 0 ? sa : sb, nullptr);
    }

    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            if (a.row(0, y)[x] != b.row(0, y)[x]) {
                microtest::fail("同じ順序の加算で結果が一致しない");
                return;
            }
        }
    }
}

MT_TEST(wstack_AP毎に別のフレーム集合を選んでも格子模様が出ない) {
    // M2の受け入れ条件「一様領域の差分検査で格子状アーティファクトが検出されない」。
    //
    // 重要: 継ぎ目の原因は窓の数式ではない。S/W で割る以上どんな窓でも
    // 振幅は合う。実際に問題になるのは、隣り合うAPが**違うフレーム集合**を
    // 選ぶことで、領域ごとに実効的なシャープさが変わることである。
    // したがって変位ゼロ・AP毎に別のフレームを選ぶ条件で試す。
    const int n = 192, ap = 32;
    const Texture tex(n, 11);
    const FrameBuffer base = sample_texture(tex, n);

    // 少しずつぼかし方の違う「フレーム」を用意する（シーイングの違いの代用）。
    std::vector<FrameBuffer> frames;
    for (int k = 0; k < 8; ++k) {
        FrameBuffer f(n, n, 1);
        Lcg rng(1000u + k);
        for (int y = 0; y < n; ++y) {
            float* row = f.row(0, y);
            for (int x = 0; x < n; ++x) {
                // フレームごとに違う量のノイズを乗せる
                const double noise = 0.02 * (k + 1) * (rng.next() - 0.5);
                row[x] = static_cast<float>(base.row(0, y)[x] + noise);
            }
        }
        f.invalidate_luma();
        frames.push_back(std::move(f));
    }

    WindowedStacker st(n, n, 1, ap);
    int ap_index = 0;
    for (int cy = ap / 2; cy + ap / 2 <= n; cy += ap / 2) {
        for (int cx = ap / 2; cx + ap / 2 <= n; cx += ap / 2) {
            st.begin_ap(cx, cy);
            // APごとに違うフレーム集合を選ぶ（spatial lucky imaging の再現）
            for (int k = 0; k < 4; ++k) {
                st.add_frame(frames[static_cast<std::size_t>((ap_index + k * 3) % 8)], 0.0, 0.0,
                             1.0);
            }
            st.end_ap();
            ++ap_index;
        }
    }

    FrameBuffer out;
    WindowedStackStats stats;
    st.finish(out, stats, &base);

    // AP間隔（=16）と APサイズ（=32）の両方の周期で調べる。
    const double ripple16 = grid_ripple_amplitude(out, ap / 2, ap);
    const double ripple32 = grid_ripple_amplitude(out, ap, ap);
    // 比較対象として、格子と無関係な周期での振幅を見る。
    const double ripple23 = grid_ripple_amplitude(out, 23, ap);

    std::printf("           格子周期の振幅: 16px=%.2e 32px=%.2e / 無関係な23px=%.2e\n",
                ripple16, ripple32, ripple23);

    // 格子ピッチの成分が、無関係な周期の成分より突出していないこと。
    // 突出していれば、それは格子に起因する周期構造がある証拠になる。
    const double baseline = ripple23 > 1e-9 ? ripple23 : 1e-9;
    if (ripple16 > baseline * 3.0 || ripple32 > baseline * 3.0) {
        microtest::fail("格子ピッチの周期成分が突出している: 16px=" +
                        microtest::mt_str(ripple16) + " 32px=" + microtest::mt_str(ripple32) +
                        " 基準=" + microtest::mt_str(ripple23));
    }
}

// --- 外れ値処理 -----------------------------------------------------------

namespace {

// 3x3のAP格子を作る。
std::vector<AlignmentPoint> grid3x3(int step) {
    std::vector<AlignmentPoint> pts;
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            AlignmentPoint p;
            p.cx = 32 + i * step;
            p.cy = 32 + j * step;
            pts.push_back(p);
        }
    }
    return pts;
}

}  // namespace

MT_TEST(repair_無効なAPは近傍の有効APから埋められる) {
    const int step = 16;
    const std::vector<AlignmentPoint> pts = grid3x3(step);
    std::vector<LocalMatch> m(pts.size());
    for (std::size_t i = 0; i < m.size(); ++i) {
        m[i].dx = 2.0f;
        m[i].dy = -1.0f;
        m[i].valid = true;
    }
    // 中央を無効にし、でたらめな値を入れておく。
    m[4].dx = 99.0f;
    m[4].dy = -99.0f;
    m[4].valid = false;

    stackcore::repair_displacement_field(pts, m, step, LocalAlignSettings{});

    MT_CHECK_NEAR(m[4].dx, 2.0f, 1e-4);
    MT_CHECK_NEAR(m[4].dy, -1.0f, 1e-4);
    MT_CHECK(!m[4].valid);  // 補間で埋めたことは記録に残る
}

MT_TEST(repair_近傍に有効APがなければゼロにする) {
    const int step = 16;
    const std::vector<AlignmentPoint> pts = grid3x3(step);
    std::vector<LocalMatch> m(pts.size());
    for (std::size_t i = 0; i < m.size(); ++i) {
        m[i].dx = 50.0f;
        m[i].dy = 50.0f;
        m[i].valid = false;
    }
    stackcore::repair_displacement_field(pts, m, step, LocalAlignSettings{});
    for (std::size_t i = 0; i < m.size(); ++i) {
        MT_CHECK_NEAR(m[i].dx, 0.0f, 1e-6);
        MT_CHECK_NEAR(m[i].dy, 0.0f, 1e-6);
    }
}

MT_TEST(repair_隣接と大きく違う変位はクリップされる) {
    const int step = 16;
    const std::vector<AlignmentPoint> pts = grid3x3(step);
    std::vector<LocalMatch> m(pts.size());
    for (std::size_t i = 0; i < m.size(); ++i) {
        m[i].dx = 1.0f;
        m[i].dy = 0.0f;
        m[i].valid = true;
    }
    m[4].dx = 20.0f;  // 近傍から大きく外れた値

    stackcore::repair_displacement_field(pts, m, step, LocalAlignSettings{});

    // 許容量は AP間隔 x 0.25 = 4px。近傍平均(1.0)から4px以内に収まるはず。
    MT_CHECK(std::fabs(m[4].dx - 1.0f) <= 4.0f + 1e-3f);
    MT_CHECK(m[4].dx > 1.0f);  // 完全に平均へ潰さず、外れた向きは残す
}

MT_TEST(repair_妥当な変位場は変更されない) {
    // 実際に局所的な歪みがある場合まで平滑化してしまってはいけない。
    const int step = 16;
    const std::vector<AlignmentPoint> pts = grid3x3(step);
    std::vector<LocalMatch> m(pts.size());
    for (std::size_t i = 0; i < m.size(); ++i) {
        // 緩やかに変化する変位場（許容量4px以内の差）
        m[i].dx = static_cast<float>(pts[i].cx) * 0.02f;
        m[i].dy = static_cast<float>(pts[i].cy) * 0.01f;
        m[i].valid = true;
    }
    const std::vector<LocalMatch> before = m;
    stackcore::repair_displacement_field(pts, m, step, LocalAlignSettings{});
    for (std::size_t i = 0; i < m.size(); ++i) {
        MT_CHECK_NEAR(m[i].dx, before[i].dx, 1e-5);
        MT_CHECK_NEAR(m[i].dy, before[i].dy, 1e-5);
    }
}
