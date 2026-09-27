// ドリズルの診断のテスト。合成した画像と変位で、判定が分かれることを確かめる。

#include <Accelerate/Accelerate.h>

#include <cmath>
#include <random>
#include <vector>

#include "microtest.hpp"
#include "stackcore/drizzle_diagnosis.hpp"

using stackcore::AnalysisData;
using stackcore::DrizzleDiagnosis;
using stackcore::FrameBuffer;
using stackcore::MapStackSettings;

namespace {

// 3倍細かい格子に模様を描き、3×3 画素ずつまとめて 256×256 にする（カメラの画素で撮ったのと同じ）。
// 模様は cutoff（仕上がりのナイキストを1とした周波数）までの成分を持ち、パワーは周波数の2乗で落ちる。
// cutoff が 1 を超えると、まとめた画像はナイキストの近くまで信号が残る粗い像になる。
// disk なら半径 90 の円の中だけに描き、外は暗い空にする。
FrameBuffer texture(double cutoff, bool disk, double noise, unsigned seed) {
    constexpr int kFine = 1024, kBin = 3, kOut = 256;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> ph(0.0, 2.0 * M_PI);
    std::normal_distribution<double> g(0.0, 1.0);
    const double f_max = cutoff * 0.5 / kBin;  // 細かい格子での cycles/pixel
    const auto freq = [](int k) {
        const int f = k < kFine / 2 ? k : k - kFine;
        return static_cast<double>(f) / kFine;
    };
    std::vector<float> re(static_cast<std::size_t>(kFine) * kFine, 0.0f), im(re.size(), 0.0f);
    for (int ky = 0; ky < kFine; ++ky) {
        for (int kx = 0; kx < kFine; ++kx) {
            const double f = std::hypot(freq(kx), freq(ky));
            if (f < 1.0 / kFine || f > f_max) continue;
            const double amp = 1.0 / (f * kFine);
            const double p = ph(rng);
            re[static_cast<std::size_t>(ky) * kFine + kx] = static_cast<float>(amp * std::cos(p));
            im[static_cast<std::size_t>(ky) * kFine + kx] = static_cast<float>(amp * std::sin(p));
        }
    }
    FFTSetup setup = vDSP_create_fftsetup(10, kFFTRadix2);
    DSPSplitComplex sc{re.data(), im.data()};
    vDSP_fft2d_zip(setup, &sc, 1, 0, 10, 10, FFT_INVERSE);
    double rms = 0.0;
    for (float v : re) rms += static_cast<double>(v) * v;
    rms = std::sqrt(rms / re.size());
    // 円で切り抜いてから、光学系と同じく cutoff より細かい周期を落とす（縁もぼける）。
    for (int y = 0; y < kFine; ++y) {
        for (int x = 0; x < kFine; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * kFine + x;
            const double dx = (x + 0.5) / kBin - 128.0, dy = (y + 0.5) / kBin - 128.0;
            const bool inside = !disk || dx * dx + dy * dy < 90.0 * 90.0;
            re[i] = inside ? static_cast<float>(0.5 + 0.1 * re[i] / rms) : 0.0f;
            im[i] = 0.0f;
        }
    }
    if (disk) {
        vDSP_fft2d_zip(setup, &sc, 1, 0, 10, 10, FFT_FORWARD);
        for (int ky = 0; ky < kFine; ++ky) {
            for (int kx = 0; kx < kFine; ++kx) {
                if (std::hypot(freq(kx), freq(ky)) <= f_max) continue;
                re[static_cast<std::size_t>(ky) * kFine + kx] = 0.0f;
                im[static_cast<std::size_t>(ky) * kFine + kx] = 0.0f;
            }
        }
        vDSP_fft2d_zip(setup, &sc, 1, 0, 10, 10, FFT_INVERSE);
        const float scale = 1.0f / (static_cast<float>(kFine) * kFine);
        for (float& v : re) v *= scale;
    }
    vDSP_destroy_fftsetup(setup);
    FrameBuffer img(kOut, kOut, 1);
    for (int y = 0; y < kOut; ++y) {
        for (int x = 0; x < kOut; ++x) {
            double v = 0.0;
            for (int j = 0; j < kBin; ++j) {
                for (int i = 0; i < kBin; ++i) v += re[static_cast<std::size_t>(y * kBin + j) * kFine + x * kBin + i];
            }
            img.row(0, y)[x] = static_cast<float>(0.02 + v / (kBin * kBin) + noise * g(rng));
        }
    }
    return img;
}

// AP 1つ・frames 枚。変位の端数は spread が真なら画素の中にまんべんなく、偽なら同じ値。
AnalysisData analysis(int frames, bool spread) {
    AnalysisData d;
    d.width = d.height = 256;
    d.channels = 1;
    d.points.resize(1);
    d.points[0].cx = 128;
    d.points[0].cy = 128;
    for (int i = 0; i < frames; ++i) d.analyzed_indices.push_back(i);
    d.matrix.resize(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        stackcore::LocalMatch& m = d.matrix[static_cast<std::size_t>(i)];
        m.dx = static_cast<float>(3.0 + (spread ? std::fmod(i * 0.618034, 1.0) : 0.25));
        m.dy = static_cast<float>(-2.0 + (spread ? std::fmod(i * 0.414214, 1.0) : 0.25));
        m.quality = 1.0f;
        m.valid = true;
    }
    return d;
}

}  // namespace

MT_TEST(drizzle診断_細かく撮れた像では等倍と判断する) {
    // 模様がナイキストの4割までしかない（十分に細かく撮れている）。
    const FrameBuffer img = texture(0.4, true, 0.002, 3);
    MapStackSettings s;
    s.ap_top_percent = 100.0;
    const DrizzleDiagnosis d = stackcore::diagnose_drizzle(analysis(400, true), img, s);
    MT_CHECK(!d.insufficient);
    MT_CHECK(d.floor_from_background);
    MT_CHECK(d.cutoff_ratio < 0.8);
    MT_CHECK_EQ(d.suggested_scale, 1.0);
    MT_CHECK_EQ(d.limiting, 0);
}

MT_TEST(drizzle診断_粗い像で位置がばらけ枚数も十分なら拡大を勧める) {
    // 模様がナイキストの2.5倍まである（粗く撮れている）。
    const FrameBuffer img = texture(2.5, true, 0.002, 5);
    MapStackSettings s;
    s.ap_top_percent = 100.0;
    const DrizzleDiagnosis d = stackcore::diagnose_drizzle(analysis(400, true), img, s);
    MT_CHECK(!d.insufficient);
    MT_CHECK(d.floor_from_background);
    MT_CHECK(d.cutoff_ratio > 1.5);
    MT_CHECK_EQ(d.suggested_scale, 2.0);
    MT_CHECK(d.phase_coverage2 >= 0.75);
}

MT_TEST(drizzle診断_位置がばらけない・枚数が少ないときは等倍) {
    const FrameBuffer img = texture(2.5, true, 0.002, 5);
    MapStackSettings s;
    s.ap_top_percent = 100.0;
    // 全フレームが同じ端数（追尾が完璧すぎる）。
    const DrizzleDiagnosis same = stackcore::diagnose_drizzle(analysis(400, false), img, s);
    MT_CHECK_EQ(same.limit_phase, 1.0);
    MT_CHECK_EQ(same.suggested_scale, 1.0);
    MT_CHECK_EQ(same.limiting, 1);
    // 採用が3枚だけ。
    const DrizzleDiagnosis few = stackcore::diagnose_drizzle(analysis(3, true), img, s);
    MT_CHECK_EQ(few.frames_per_ap, 3);
    MT_CHECK_EQ(few.suggested_scale, 1.0);
    // 解析結果が無ければ判断できない。
    const DrizzleDiagnosis none = stackcore::diagnose_drizzle(AnalysisData(), img, s);
    MT_CHECK(none.insufficient);
}

MT_TEST(drizzle診断_ナイキストの近くで落ちる像は測った位置を返す) {
    MapStackSettings s;
    s.ap_top_percent = 100.0;
    for (double c : {0.8, 1.2}) {
        const DrizzleDiagnosis d = stackcore::diagnose_drizzle(analysis(400, true), texture(c, true, 0.002, 9), s);
        MT_CHECK(!d.cutoff_extrapolated);
        MT_CHECK(std::fabs(d.cutoff_ratio - c) < 0.1);
    }
}

MT_TEST(drizzle診断_空が写っていなければ控えめに見積もる) {
    // 月面の全面のように、画面全体に模様がある（ノイズの水準を空から測れない）。
    const FrameBuffer img = texture(2.5, false, 0.002, 7);
    MapStackSettings s;
    s.ap_top_percent = 100.0;
    const DrizzleDiagnosis d = stackcore::diagnose_drizzle(analysis(400, true), img, s);
    MT_CHECK(!d.insufficient);
    MT_CHECK(!d.floor_from_background);
    MT_CHECK(d.cutoff_ratio <= 1.5);
}
