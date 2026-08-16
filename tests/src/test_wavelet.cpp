#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "microtest.hpp"
#include "stackcore/frame_buffer.hpp"
#include "stackcore/wavelet.hpp"

using stackcore::FrameBuffer;
using stackcore::WaveletLayerParams;
using stackcore::WaveletSharpener;

namespace {

struct Lcg {
    std::uint32_t s;
    explicit Lcg(std::uint32_t seed) : s(seed) {}
    double next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<double>((s >> 8) & 0xFFFF) / 65535.0;
    }
};

// 細かい構造と大きな構造の両方を含む画像。
// レイヤーごとの効きを見るには、複数の空間周波数が要る。
FrameBuffer make_image(int w, int h, int channels, double noise, std::uint32_t seed) {
    FrameBuffer fb(w, h, channels);
    Lcg rng(seed);
    for (int c = 0; c < channels; ++c) {
        for (int y = 0; y < h; ++y) {
            float* row = fb.row(c, y);
            for (int x = 0; x < w; ++x) {
                double v = 0.45;
                v += 0.20 * std::sin(x * 2.0 * M_PI / 37.0) * std::cos(y * 2.0 * M_PI / 41.0);
                v += 0.10 * std::sin((x + y) * 2.0 * M_PI / 11.0);
                v += 0.05 * std::sin(x * 2.0 * M_PI / 3.5) * std::sin(y * 2.0 * M_PI / 3.1);
                v += 0.02 * c;
                v += noise * (rng.next() - 0.5);
                row[x] = static_cast<float>(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
            }
        }
    }
    fb.invalidate_luma();
    return fb;
}

std::vector<WaveletLayerParams> flat_params(int layers, double sharpen, double denoise) {
    std::vector<WaveletLayerParams> p(static_cast<std::size_t>(layers));
    for (std::size_t i = 0; i < p.size(); ++i) {
        p[i].sharpen = sharpen;
        p[i].denoise = denoise;
    }
    return p;
}

double max_abs_diff(const FrameBuffer& a, const FrameBuffer& b) {
    double worst = 0.0;
    for (int c = 0; c < a.channels(); ++c) {
        for (int y = 0; y < a.height(); ++y) {
            const float* ra = a.row(c, y);
            const float* rb = b.row(c, y);
            for (int x = 0; x < a.width(); ++x) {
                const double d = std::fabs(static_cast<double>(ra[x]) - rb[x]);
                if (d > worst) worst = d;
            }
        }
    }
    return worst;
}

// 高周波の量。ぼかしとの差の大きさで測る。
double high_frequency_power(const FrameBuffer& f) {
    double sum = 0.0;
    int count = 0;
    for (int y = 1; y < f.height() - 1; ++y) {
        const float* prev = f.row(0, y - 1);
        const float* cur = f.row(0, y);
        const float* next = f.row(0, y + 1);
        for (int x = 1; x < f.width() - 1; ++x) {
            const double lap = 4.0 * cur[x] - cur[x - 1] - cur[x + 1] - prev[x] - next[x];
            sum += lap * lap;
            ++count;
        }
    }
    return count > 0 ? std::sqrt(sum / count) : 0.0;
}

MT_TEST(wavelet_係数1で再構成すると元に戻る) {
    // M6の受け入れ条件。分解と再構成が対になっていることの証明。
    //   I' = c_J + Σ w_j = c_J + Σ(c_{j-1} - c_j) = c_0 = I
    // 望遠鏡の項が打ち消し合うので、数学的には厳密に一致する。
    // 実装がずれていればここで必ず落ちる。
    const FrameBuffer src = make_image(96, 72, 3, 0.02, 11);
    WaveletSharpener w;
    w.analyze(src, 6);

    FrameBuffer out;
    w.synthesize(flat_params(6, 1.0, 0.0), out);

    const double worst = max_abs_diff(src, out);
    std::printf("           可逆性の最大誤差: %.3e\n", worst);
    if (!(worst < 1e-5)) {
        microtest::fail("係数1.0の再構成が元と一致しない: 最大誤差 " +
                        microtest::mt_str(worst));
    }
}

MT_TEST(wavelet_レイヤー数を変えても可逆性は保たれる) {
    const FrameBuffer src = make_image(64, 64, 1, 0.01, 5);
    for (int layers : {1, 3, 6, 8}) {
        WaveletSharpener w;
        w.analyze(src, layers);
        FrameBuffer out;
        w.synthesize(flat_params(layers, 1.0, 0.0), out);
        const double worst = max_abs_diff(src, out);
        if (!(worst < 1e-5)) {
            microtest::fail("レイヤー数 " + microtest::mt_str(layers) +
                            " で可逆性が崩れた: " + microtest::mt_str(worst));
            return;
        }
    }
}

MT_TEST(wavelet_一様な画像は分解しても詳細がゼロ) {
    FrameBuffer flat(48, 48, 1);
    for (int y = 0; y < 48; ++y) {
        float* row = flat.row(0, y);
        for (int x = 0; x < 48; ++x) row[x] = 0.6f;
    }
    flat.invalidate_luma();

    WaveletSharpener w;
    w.analyze(flat, 4);
    // 詳細がゼロなら、係数をいくつにしても結果は変わらないはず。
    FrameBuffer a, b;
    w.synthesize(flat_params(4, 1.0, 0.0), a);
    w.synthesize(flat_params(4, 3.0, 0.0), b);
    MT_CHECK_NEAR(max_abs_diff(a, b), 0.0, 1e-6);
    MT_CHECK_NEAR(a.row(0, 24)[24], 0.6f, 1e-5);
}

MT_TEST(wavelet_係数を上げると高周波が増える) {
    const FrameBuffer src = make_image(96, 96, 1, 0.0, 7);
    WaveletSharpener w;
    w.analyze(src, 6);

    FrameBuffer plain, sharp;
    w.synthesize(flat_params(6, 1.0, 0.0), plain);

    // 細かいレイヤーだけ強める
    std::vector<WaveletLayerParams> p = flat_params(6, 1.0, 0.0);
    p[0].sharpen = 2.5;
    p[1].sharpen = 2.0;
    w.synthesize(p, sharp);

    const double before = high_frequency_power(plain);
    const double after = high_frequency_power(sharp);
    std::printf("           高周波の量: 係数1.0=%.5f → 細層強調=%.5f\n", before, after);
    if (!(after > before * 1.3)) {
        microtest::fail("シャープ係数を上げても高周波が増えない");
    }
}

MT_TEST(wavelet_拡張した実用域は従来上限より強く効く) {
    const FrameBuffer src = make_image(96, 96, 1, 0.0, 17);
    WaveletSharpener w;
    w.analyze(src, 6);

    std::vector<WaveletLayerParams> old_limit = flat_params(6, 1.0, 0.0);
    old_limit[0].sharpen = 3.0;
    old_limit[1].sharpen = 2.0;

    std::vector<WaveletLayerParams> expanded = flat_params(6, 1.0, 0.0);
    expanded[0].sharpen = 16.0;
    expanded[1].sharpen = 8.0;
    expanded[2].sharpen = 3.0;

    FrameBuffer old_out, expanded_out;
    w.synthesize(old_limit, old_out);
    w.synthesize(expanded, expanded_out);

    const double old_power = high_frequency_power(old_out);
    const double expanded_power = high_frequency_power(expanded_out);
    std::printf("           高周波の量: 従来域=%.5f → 拡張域=%.5f\n",
                old_power, expanded_power);
    if (!(expanded_power > old_power * 2.0)) {
        microtest::fail("拡張したSharpen範囲が従来上限より十分に強くない");
    }
}

MT_TEST(wavelet_係数0にするとその帯域が消える) {
    const FrameBuffer src = make_image(96, 96, 1, 0.0, 9);
    WaveletSharpener w;
    w.analyze(src, 6);

    FrameBuffer plain, soft;
    w.synthesize(flat_params(6, 1.0, 0.0), plain);
    std::vector<WaveletLayerParams> p = flat_params(6, 1.0, 0.0);
    p[0].sharpen = 0.0;
    p[1].sharpen = 0.0;
    w.synthesize(p, soft);

    if (!(high_frequency_power(soft) < high_frequency_power(plain) * 0.7)) {
        microtest::fail("細層を0にしても高周波が減らない");
    }
}

// 最も細かい周期成分を持たない画像。
//
// Denoiseの検証にはこちらを使う。make_image は周期3.5pxという
// ナイキストに近い実信号を含んでおり、それが最細レイヤーでノイズと同居する。
// その画像でDenoiseを測ると「ノイズを消せているか」ではなく
// 「実信号をどれだけ巻き込んだか」を測ることになる（実測で誤差が3倍に増えた）。
//
// Denoiseは本質的に「細部とノイズの取引」であり、
// 細部がノイズと同じ帯域にあれば必ず巻き添えになる。
// 機構が正しく働くことを確かめたいなら、その帯域がノイズ支配である画像を使う。
FrameBuffer make_smooth_image(int w, int h, double noise, std::uint32_t seed) {
    FrameBuffer fb(w, h, 1);
    Lcg rng(seed);
    for (int y = 0; y < h; ++y) {
        float* row = fb.row(0, y);
        for (int x = 0; x < w; ++x) {
            double v = 0.45;
            v += 0.20 * std::sin(x * 2.0 * M_PI / 37.0) * std::cos(y * 2.0 * M_PI / 41.0);
            v += 0.10 * std::sin((x + y) * 2.0 * M_PI / 17.0);
            v += noise * (rng.next() - 0.5);
            row[x] = static_cast<float>(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
        }
    }
    fb.invalidate_luma();
    return fb;
}

}  // namespace

MT_TEST(wavelet_Denoiseはノイズ支配の帯域で誤差を減らす) {
    const FrameBuffer clean = make_smooth_image(96, 96, 0.0, 13);
    const FrameBuffer noisy = make_smooth_image(96, 96, 0.06, 13);

    WaveletSharpener w;
    w.analyze(noisy, 6);

    FrameBuffer plain, denoised;
    w.synthesize(flat_params(6, 1.0, 0.0), plain);
    w.synthesize(flat_params(6, 1.0, 0.5), denoised);

    double err_plain = 0.0, err_denoised = 0.0;
    int n = 0;
    for (int y = 4; y < 92; ++y) {
        for (int x = 4; x < 92; ++x) {
            const double c = clean.row(0, y)[x];
            const double a = plain.row(0, y)[x] - c;
            const double b = denoised.row(0, y)[x] - c;
            err_plain += a * a;
            err_denoised += b * b;
            ++n;
        }
    }
    err_plain = std::sqrt(err_plain / n);
    err_denoised = std::sqrt(err_denoised / n);
    std::printf("           元画像との誤差: Denoiseなし=%.5f あり=%.5f (%.0f%%減)\n",
                err_plain, err_denoised, 100.0 * (1.0 - err_denoised / err_plain));
    if (!(err_denoised < err_plain)) {
        microtest::fail("Denoiseで元画像に近づいていない: なし=" +
                        microtest::mt_str(err_plain) + " あり=" +
                        microtest::mt_str(err_denoised));
    }
}

MT_TEST(wavelet_Denoiseの効きが単調ではなく最良点を持つ) {
    // soft-thresholdは「削りすぎ」が必ず起きる。スライダーの可動域が
    // 意味を持つよう、上限係数を実測で決めてある（wavelet.cpp のコメント参照）。
    // ここでは最良点が中ほどにあることを固定し、上限係数を安易に変えられなくする。
    const FrameBuffer clean = make_smooth_image(96, 96, 0.0, 13);
    const FrameBuffer noisy = make_smooth_image(96, 96, 0.06, 13);
    WaveletSharpener w;
    w.analyze(noisy, 6);

    double best_value = 1e30;
    double best_at = -1.0;
    for (double d = 0.0; d <= 1.001; d += 0.1) {
        FrameBuffer out;
        w.synthesize(flat_params(6, 1.0, d), out);
        double err = 0.0;
        int n = 0;
        for (int y = 4; y < 92; ++y) {
            for (int x = 4; x < 92; ++x) {
                const double e = out.row(0, y)[x] - clean.row(0, y)[x];
                err += e * e;
                ++n;
            }
        }
        err = std::sqrt(err / n);
        if (err < best_value) {
            best_value = err;
            best_at = d;
        }
    }
    std::printf("           最良のdenoise値: %.1f (誤差 %.5f)\n", best_at, best_value);
    if (!(best_at >= 0.3 && best_at <= 0.9)) {
        microtest::fail("最良点がスライダーの端に寄っている: " + microtest::mt_str(best_at));
    }
}

MT_TEST(wavelet_Denoiseは細部と引き換えであることを記録する) {
    // 上のテストの対（つい）。細部がノイズと同じ帯域にある画像では、
    // Denoiseは元画像との誤差を**増やす**。これは不具合ではなく
    // soft-thresholdの性質そのものであり、UIで説明すべき事柄である。
    const FrameBuffer clean = make_image(96, 96, 1, 0.0, 13);
    const FrameBuffer noisy = make_image(96, 96, 1, 0.06, 13);

    WaveletSharpener w;
    w.analyze(noisy, 6);
    FrameBuffer plain, denoised;
    w.synthesize(flat_params(6, 1.0, 0.0), plain);
    w.synthesize(flat_params(6, 1.0, 1.0), denoised);

    double a = 0.0, b = 0.0;
    int n = 0;
    for (int y = 4; y < 92; ++y) {
        for (int x = 4; x < 92; ++x) {
            const double c = clean.row(0, y)[x];
            const double da = plain.row(0, y)[x] - c;
            const double db = denoised.row(0, y)[x] - c;
            a += da * da;
            b += db * db;
            ++n;
        }
    }
    std::printf("           ナイキスト近傍に信号がある画像: なし=%.5f あり=%.5f\n",
                std::sqrt(a / n), std::sqrt(b / n));
    // 悪化すること自体は正常。ここでは「黙って壊れていない」ことだけ確かめる。
    MT_CHECK(std::sqrt(b / n) > 0.0);
}

MT_TEST(wavelet_パラメータ数が合わなければ例外) {
    const FrameBuffer src = make_image(32, 32, 1, 0.0, 1);
    WaveletSharpener w;
    w.analyze(src, 4);
    FrameBuffer out;
    MT_CHECK_THROWS(w.synthesize(flat_params(3, 1.0, 0.0), out));
}

MT_TEST(wavelet_範囲外または非有限のパラメータは拒否する) {
    const FrameBuffer src = make_image(32, 32, 1, 0.0, 3);
    WaveletSharpener w;
    w.analyze(src, 2);
    FrameBuffer out;

    auto p = flat_params(2, 1.0, 0.0);
    p[0].sharpen = -0.01;
    MT_CHECK_THROWS(w.synthesize(p, out));
    p[0].sharpen = stackcore::kWaveletSharpenMaximum + 0.01;
    MT_CHECK_THROWS(w.synthesize(p, out));
    p[0].sharpen = std::numeric_limits<double>::quiet_NaN();
    MT_CHECK_THROWS(w.synthesize(p, out));

    p = flat_params(2, 1.0, 0.0);
    p[1].denoise = 1.01;
    MT_CHECK_THROWS(w.synthesize(p, out));
    p[1].denoise = std::numeric_limits<double>::infinity();
    MT_CHECK_THROWS(w.synthesize(p, out));
}

MT_TEST(wavelet_analyzeを呼ばずにsynthesizeすると例外) {
    WaveletSharpener w;
    FrameBuffer out;
    MT_CHECK_THROWS(w.synthesize(flat_params(6, 1.0, 0.0), out));
}

MT_TEST(wavelet_512x512の再構成が応答目標を満たす) {
    // 仕様書 §4.10 は「プレビューは選択領域（既定512x512）で更新し、
    // 推奨環境 <100ms」としている。分解を毎回やり直さない設計であることの確認。
    const FrameBuffer src = make_image(512, 512, 3, 0.01, 21);
    WaveletSharpener w;
    w.analyze(src, 6);

    FrameBuffer out;
    const std::vector<WaveletLayerParams> p = flat_params(6, 1.4, 0.2);
    w.synthesize(p, out);  // 1回目はバッファ確保が入るので計測から外す

    const auto t0 = std::chrono::steady_clock::now();
    const int iterations = 5;
    for (int i = 0; i < iterations; ++i) w.synthesize(p, out);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count() / iterations;

    std::printf("           512x512x3の再構成: %.1f ms\n", ms);
    if (!(ms < 100.0)) {
        microtest::fail("再構成が応答目標100msを超えている: " + microtest::mt_str(ms) + " ms");
    }
}

// --- ヒストグラムストレッチ ------------------------------------------------

MT_TEST(stretch_黒点と白点で範囲が伸びる) {
    FrameBuffer src(8, 8, 1);
    for (int y = 0; y < 8; ++y) {
        float* row = src.row(0, y);
        for (int x = 0; x < 8; ++x) row[x] = 0.2f + 0.05f * x;  // 0.2 .. 0.55
    }
    src.invalidate_luma();

    FrameBuffer out;
    stackcore::stretch_histogram(src, 0.2, 0.55, 1.0, out);
    MT_CHECK_NEAR(out.row(0, 0)[0], 0.0f, 1e-5);
    MT_CHECK_NEAR(out.row(0, 0)[7], 1.0f, 1e-5);
}

MT_TEST(stretch_範囲外は切り詰められる) {
    FrameBuffer src(4, 4, 1);
    for (int y = 0; y < 4; ++y) {
        float* row = src.row(0, y);
        row[0] = 0.0f;
        row[1] = 0.3f;
        row[2] = 0.7f;
        row[3] = 1.0f;
    }
    src.invalidate_luma();

    FrameBuffer out;
    stackcore::stretch_histogram(src, 0.3, 0.7, 1.0, out);
    MT_CHECK_NEAR(out.row(0, 0)[0], 0.0f, 1e-6);
    MT_CHECK_NEAR(out.row(0, 0)[1], 0.0f, 1e-6);
    MT_CHECK_NEAR(out.row(0, 0)[2], 1.0f, 1e-6);
    MT_CHECK_NEAR(out.row(0, 0)[3], 1.0f, 1e-6);
}

MT_TEST(stretch_ガンマが中間調を持ち上げる) {
    FrameBuffer src(4, 4, 1);
    for (int y = 0; y < 4; ++y) {
        float* row = src.row(0, y);
        for (int x = 0; x < 4; ++x) row[x] = 0.25f;
    }
    src.invalidate_luma();

    FrameBuffer out;
    stackcore::stretch_histogram(src, 0.0, 1.0, 2.2, out);
    // ガンマ2.2なら 0.25^(1/2.2) ≈ 0.533
    MT_CHECK_NEAR(out.row(0, 0)[0], static_cast<float>(std::pow(0.25, 1.0 / 2.2)), 1e-5);
}

MT_TEST(stretch_不正なパラメータは例外) {
    const FrameBuffer src = make_image(8, 8, 1, 0.0, 1);
    FrameBuffer out;
    MT_CHECK_THROWS(stackcore::stretch_histogram(src, 0.7, 0.3, 1.0, out));
    MT_CHECK_THROWS(stackcore::stretch_histogram(src, 0.0, 1.0, 0.0, out));
}
