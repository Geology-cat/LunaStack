#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "microtest.hpp"
#include "stackcore/map_pipeline.hpp"
#include "stackcore/sidecar.hpp"
#include "stackcore/video_source.hpp"

using stackcore::FrameBuffer;
using stackcore::FrameStats;
using stackcore::MapStackReport;
using stackcore::MapStackSettings;
using stackcore::SerColorId;
using stackcore::VideoSource;

namespace {

struct Lcg {
    std::uint32_t s;
    explicit Lcg(std::uint32_t seed) : s(seed) {}
    double next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<double>((s >> 8) & 0xFFFF) / 65535.0;
    }
};

// 連続関数として定義したシーン（ガウス斑点の重ね合わせ）。
// 非周期にするため三角関数の重ね合わせは使わない。
struct Scene {
    struct Blob {
        double x, y, a;
    };
    std::vector<Blob> blobs;
    double sigma = 1.6;

    Scene(int extent, std::uint32_t seed) {
        Lcg rng(seed);
        const int n = extent * extent / 60;
        for (int i = 0; i < n; ++i) {
            Blob b;
            b.x = rng.next() * (extent + 40.0) - 20.0;
            b.y = rng.next() * (extent + 40.0) - 20.0;
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
        return 0.15 + 0.20 * v;
    }
};

// 低周波の滑らかな変位場。シーイングによる歪みの代用。
// 波長をAPサイズより十分長く取るので、AP内ではほぼ一様な平行移動に見える。
struct Warp {
    struct Wave {
        double k, angle, phase;
    };
    std::vector<Wave> wx, wy;
    double amplitude;

    Warp(double amp, std::uint32_t seed) : amplitude(amp) {
        Lcg rng(seed);
        for (int i = 0; i < 3; ++i) {
            Wave a;
            a.k = 2.0 * M_PI / (60.0 + rng.next() * 60.0);
            a.angle = rng.next() * 2.0 * M_PI;
            a.phase = rng.next() * 2.0 * M_PI;
            wx.push_back(a);
            Wave b;
            b.k = 2.0 * M_PI / (60.0 + rng.next() * 60.0);
            b.angle = rng.next() * 2.0 * M_PI;
            b.phase = rng.next() * 2.0 * M_PI;
            wy.push_back(b);
        }
    }

    static double eval(const std::vector<Wave>& w, double x, double y) {
        double v = 0.0;
        for (std::size_t i = 0; i < w.size(); ++i) {
            const double proj = x * std::cos(w[i].angle) + y * std::sin(w[i].angle);
            v += std::sin(w[i].k * proj + w[i].phase);
        }
        return v / w.size();
    }

    void at(double x, double y, double& dx, double& dy) const {
        dx = amplitude * eval(wx, x, y);
        dy = amplitude * eval(wy, x, y);
    }
};

// メモリ上で合成フレームを供給する VideoSource。
class SyntheticSource : public VideoSource {
public:
    SyntheticSource(int size, int frames, double local_amp, double global_amp,
                    double noise, std::uint32_t seed)
        : size_(size), scene_(size, seed) {
        Lcg rng(seed + 7);
        frames_.reserve(static_cast<std::size_t>(frames));
        for (int n = 0; n < frames; ++n) {
            const double gx = (rng.next() - 0.5) * 2.0 * global_amp;
            const double gy = (rng.next() - 0.5) * 2.0 * global_amp;
            const Warp warp(local_amp, seed + 1000 + n);

            FrameBuffer f(size, size, 1);
            Lcg nrng(seed + 5000 + n);
            for (int y = 0; y < size; ++y) {
                float* row = f.row(0, y);
                for (int x = 0; x < size; ++x) {
                    double ldx = 0.0, ldy = 0.0;
                    if (local_amp > 0.0) warp.at(x, y, ldx, ldy);
                    // 歪んだ座標で連続関数を直接評価する。
                    // 画像を作ってからリサンプルすると、その誤差が混ざる。
                    double v = scene_.at(x + gx + ldx, y + gy + ldy);
                    v += noise * (nrng.next() - 0.5);
                    row[x] = static_cast<float>(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
                }
            }
            f.invalidate_luma();
            frames_.push_back(std::move(f));
        }
    }

    // 歪みなしで標本化した真値。
    FrameBuffer truth() const {
        FrameBuffer f(size_, size_, 1);
        for (int y = 0; y < size_; ++y) {
            float* row = f.row(0, y);
            for (int x = 0; x < size_; ++x) {
                const double v = scene_.at(x, y);
                row[x] = static_cast<float>(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
            }
        }
        f.invalidate_luma();
        return f;
    }

    int width() const override { return size_; }
    int height() const override { return size_; }
    int frame_count() const override { return static_cast<int>(frames_.size()); }
    SerColorId color_id() const override { return SerColorId::Mono; }
    int bit_depth() const override { return 16; }
    bool has_timestamps() const override { return false; }
    std::int64_t timestamp_ticks(int) const override { return 0; }
    FrameStats frame_stats(int) const override { return FrameStats{}; }
    const char* format_name() const override { return "synthetic"; }
    std::string describe() const override { return "合成"; }

    void read_frame(int index, FrameBuffer& out) const override {
        const FrameBuffer& src = frames_[static_cast<std::size_t>(index)];
        if (out.width() != size_ || out.height() != size_ || out.channels() != 1) {
            out.reset(size_, size_, 1);
        }
        for (int y = 0; y < size_; ++y) {
            const float* s = src.row(0, y);
            float* d = out.row(0, y);
            for (int x = 0; x < size_; ++x) d[x] = s[x];
        }
        out.invalidate_luma();
    }

private:
    int size_;
    Scene scene_;
    std::vector<FrameBuffer> frames_;
};

// 平行移動のずれを許した上での、真値との最小RMS。
// 出力は参照フレームの座標系にあり、真値とは全体的にずれている。
// そのずれを含めて測ると、局所の合い方の差が埋もれてしまう。
double rms_vs_truth(const FrameBuffer& img, const FrameBuffer& truth, int margin,
                    int search) {
    const int n = img.width();
    double best = 1e30;
    for (int dy = -search; dy <= search; ++dy) {
        for (int dx = -search; dx <= search; ++dx) {
            double sa = 0.0, sb = 0.0, saa = 0.0, sab = 0.0;
            int count = 0;
            for (int y = margin; y < n - margin; ++y) {
                for (int x = margin; x < n - margin; ++x) {
                    const int sx = x + dx, sy = y + dy;
                    if (sx < 0 || sy < 0 || sx >= n || sy >= n) continue;
                    const double a = img.row(0, sy)[sx];
                    const double b = truth.row(0, y)[x];
                    sa += a;
                    sb += b;
                    saa += a * a;
                    sab += a * b;
                    ++count;
                }
            }
            if (count < 16) continue;
            // 明るさ・コントラストの違いを最小二乗で吸収してから比べる。
            const double denom = count * saa - sa * sa;
            if (std::fabs(denom) < 1e-12) continue;
            const double k = (count * sab - sa * sb) / denom;
            const double c = (sb - k * sa) / count;

            double err = 0.0;
            for (int y = margin; y < n - margin; ++y) {
                for (int x = margin; x < n - margin; ++x) {
                    const int sx = x + dx, sy = y + dy;
                    if (sx < 0 || sy < 0 || sx >= n || sy >= n) continue;
                    const double d = k * img.row(0, sy)[sx] + c - truth.row(0, y)[x];
                    err += d * d;
                }
            }
            const double rms = std::sqrt(err / count);
            if (rms < best) best = rms;
        }
    }
    return best;
}

}  // namespace

MT_TEST(map_品質評価とアライメントを分けても一括処理と一致する) {
    // GUIが工程ごとに停止しても、従来の一括処理と全く同じ解析結果になることを
    // 保証する。ここが崩れると、「解析」ボタンを分けただけで画質が変わってしまう。
    const int size = 96;
    const SyntheticSource source(size, 24, 2.0, 1.0, 0.02, 8181);

    MapStackSettings settings;
    settings.reference_top_percent = 60.0;
    settings.ap_top_percent = 50.0;
    settings.ap.ap_size = 32;
    settings.local.search_radius = 8;
    settings.reference_passes = 2;

    const stackcore::GlobalStageReport quality =
        stackcore::evaluate_frame_quality(source, settings.global, settings.raw_cfa, nullptr);
    MT_CHECK_EQ(static_cast<int>(quality.frames.size()), source.frame_count());
    for (std::size_t i = 0; i < quality.frames.size(); ++i) {
        MT_CHECK(quality.frames[i].accepted);
        MT_CHECK_EQ(quality.frames[i].dx, 0);
        MT_CHECK_EQ(quality.frames[i].dy, 0);
        MT_CHECK_EQ(quality.frames[i].similarity, 0.0);
    }

    const stackcore::GlobalStageReport aligned = stackcore::run_global_alignment(
        source, settings.global, settings.raw_cfa, quality, nullptr);
    MT_CHECK_EQ(aligned.reference_index, quality.reference_index);
    MT_CHECK_EQ(aligned.reference_mean, quality.reference_mean);
    MT_CHECK_EQ(aligned.frames.size(), quality.frames.size());
    for (std::size_t i = 0; i < quality.frames.size(); ++i) {
        MT_CHECK_EQ(aligned.frames[i].index, quality.frames[i].index);
        MT_CHECK_EQ(aligned.frames[i].quality, quality.frames[i].quality);
        MT_CHECK_EQ(aligned.frames[i].mean, quality.frames[i].mean);
    }

    MapStackReport staged_report;
    const stackcore::AnalysisData staged =
        stackcore::analyze_map_alignment(source, settings, aligned, nullptr, staged_report);
    MapStackReport combined_report;
    const stackcore::AnalysisData combined =
        stackcore::analyze_map_stack(source, settings, nullptr, combined_report);

    MT_CHECK_EQ(staged.reference_index, combined.reference_index);
    MT_CHECK_EQ(staged.reference_mean, combined.reference_mean);
    MT_CHECK_EQ(staged.width, combined.width);
    MT_CHECK_EQ(staged.height, combined.height);
    MT_CHECK_EQ(staged.channels, combined.channels);
    MT_CHECK_EQ(staged.ap_size, combined.ap_size);
    MT_CHECK_EQ(staged.ap_grid_step, combined.ap_grid_step);
    MT_CHECK_EQ(staged.frames.size(), combined.frames.size());
    MT_CHECK_EQ(staged.points.size(), combined.points.size());
    MT_CHECK_EQ(staged.analyzed_indices.size(), combined.analyzed_indices.size());
    MT_CHECK_EQ(staged.matrix.size(), combined.matrix.size());
    MT_CHECK_EQ(staged.reference.size(), combined.reference.size());

    for (std::size_t i = 0; i < staged.frames.size(); ++i) {
        const stackcore::FrameInfo& a = staged.frames[i];
        const stackcore::FrameInfo& b = combined.frames[i];
        MT_CHECK_EQ(a.index, b.index);
        MT_CHECK_EQ(a.quality, b.quality);
        MT_CHECK_EQ(a.mean, b.mean);
        MT_CHECK_EQ(a.dx, b.dx);
        MT_CHECK_EQ(a.dy, b.dy);
        MT_CHECK_EQ(a.similarity, b.similarity);
        MT_CHECK_EQ(a.accepted, b.accepted);
        MT_CHECK_EQ(static_cast<int>(a.reason), static_cast<int>(b.reason));
    }
    for (std::size_t i = 0; i < staged.points.size(); ++i) {
        MT_CHECK_EQ(staged.points[i].cx, combined.points[i].cx);
        MT_CHECK_EQ(staged.points[i].cy, combined.points[i].cy);
        MT_CHECK_EQ(staged.points[i].mean_gradient, combined.points[i].mean_gradient);
        MT_CHECK_EQ(staged.points[i].mean_level, combined.points[i].mean_level);
        MT_CHECK_EQ(staged.points[i].min_eigenvalue, combined.points[i].min_eigenvalue);
    }
    for (std::size_t i = 0; i < staged.matrix.size(); ++i) {
        MT_CHECK_EQ(staged.matrix[i].dx, combined.matrix[i].dx);
        MT_CHECK_EQ(staged.matrix[i].dy, combined.matrix[i].dy);
        MT_CHECK_EQ(staged.matrix[i].score, combined.matrix[i].score);
        MT_CHECK_EQ(staged.matrix[i].quality, combined.matrix[i].quality);
        MT_CHECK_EQ(staged.matrix[i].valid, combined.matrix[i].valid);
    }
    MT_CHECK(staged.analyzed_indices == combined.analyzed_indices);
    MT_CHECK(staged.reference == combined.reference);

    MapStackReport staged_stack_report, combined_stack_report;
    const FrameBuffer staged_image = stackcore::stack_from_analysis(
        source, settings, staged, nullptr, staged_stack_report);
    const FrameBuffer combined_image = stackcore::stack_from_analysis(
        source, settings, combined, nullptr, combined_stack_report);
    MT_CHECK_EQ(staged_image.width(), combined_image.width());
    MT_CHECK_EQ(staged_image.height(), combined_image.height());
    MT_CHECK_EQ(staged_image.channels(), combined_image.channels());
    for (int c = 0; c < staged_image.channels(); ++c) {
        for (int y = 0; y < staged_image.height(); ++y) {
            for (int x = 0; x < staged_image.width(); ++x) {
                if (staged_image.row(c, y)[x] != combined_image.row(c, y)[x]) {
                    microtest::fail("段階処理のスタックが一括処理と一致しない (" +
                                    microtest::mt_str(x) + "," + microtest::mt_str(y) + ")");
                    return;
                }
            }
        }
    }
}

MT_TEST(map_既知の局所歪みをM1より正確に取り除く) {
    // M2の受け入れ条件「M1の単純スタックより細部が改善」を、
    // 歪みの量が分かっている合成データで測る。
    //
    // 実データ（PIPP処理済みの木星）ではこの差が出なかった。
    // それが実装の問題なのか素材に局所歪みが無いせいなのかを
    // 切り分けられるのは、歪みを自分で入れた動画だけである。
    const int size = 128;
    const SyntheticSource source(size, 60, 2.5, 1.5, 0.02, 4242);
    const FrameBuffer truth = source.truth();

    MapStackSettings settings;
    settings.reference_top_percent = 60.0;
    settings.ap_top_percent = 60.0;
    settings.ap.ap_size = 32;
    settings.local.search_radius = 8;

    MapStackReport report;
    const FrameBuffer m2 = stackcore::run_map_stack(source, settings, nullptr, report);

    // 同じ選択条件でのM1（グローバルのみ）。
    const std::vector<stackcore::FrameInfo> selected =
        stackcore::select_top_frames(report.global.frames, settings.reference_top_percent);
    const FrameBuffer m1 =
        stackcore::build_global_reference(source, report.global, selected, false, nullptr);

    const double rms_m1 = rms_vs_truth(m1, truth, 24, 4);
    const double rms_m2 = rms_vs_truth(m2, truth, 24, 4);

    std::printf("           AP=%d個 / 無効 %lld / 真値とのRMS: M1=%.5f M2=%.5f (%.0f%%減)\n",
                report.ap_count, report.invalid_matches, rms_m1, rms_m2,
                100.0 * (1.0 - rms_m2 / rms_m1));

    if (!(rms_m2 < rms_m1 * 0.9)) {
        microtest::fail("局所歪みがあるのにMAPがM1を10%以上改善していない: M1=" +
                        microtest::mt_str(rms_m1) + " M2=" + microtest::mt_str(rms_m2));
    }
}

MT_TEST(map_局所歪みがなければM1と大差ない) {
    // 対照実験。グローバル平行移動しかない素材では、MAPに取り除くものが無い。
    // ここで大きく改善するようなら、それは歪み補正ではなく別の効果
    // （選択の違いなど）を測ってしまっている証拠になる。
    const int size = 128;
    const SyntheticSource source(size, 60, 0.0, 1.5, 0.02, 777);
    const FrameBuffer truth = source.truth();

    MapStackSettings settings;
    settings.reference_top_percent = 60.0;
    settings.ap_top_percent = 60.0;
    settings.ap.ap_size = 32;
    settings.local.search_radius = 8;

    MapStackReport report;
    const FrameBuffer m2 = stackcore::run_map_stack(source, settings, nullptr, report);
    const std::vector<stackcore::FrameInfo> selected =
        stackcore::select_top_frames(report.global.frames, settings.reference_top_percent);
    const FrameBuffer m1 =
        stackcore::build_global_reference(source, report.global, selected, false, nullptr);

    const double rms_m1 = rms_vs_truth(m1, truth, 24, 4);
    const double rms_m2 = rms_vs_truth(m2, truth, 24, 4);
    std::printf("           歪みなし: M1=%.5f M2=%.5f\n", rms_m1, rms_m2);

    // MAPが悪化させないことも確認しておく（余計な補間で崩れないこと）。
    if (!(rms_m2 < rms_m1 * 1.15)) {
        microtest::fail("歪みがないのにMAPが悪化させている: M1=" + microtest::mt_str(rms_m1) +
                        " M2=" + microtest::mt_str(rms_m2));
    }
}

MT_TEST(map_参照の反復精密化が結果を改善する) {
    // 仕様書 §4.4 の「参照の反復精密化」（既定ON）。
    // 暫定参照はグローバル整数変位の平均であり、局所歪みが平均されて残っている。
    // MAPの結果を新しい参照にすればテンプレートの質が上がるはず。
    const int size = 128;
    const SyntheticSource source(size, 60, 2.5, 1.5, 0.02, 4242);
    const FrameBuffer truth = source.truth();

    MapStackSettings settings;
    settings.reference_top_percent = 60.0;
    settings.ap_top_percent = 60.0;
    settings.ap.ap_size = 32;
    settings.local.search_radius = 8;

    settings.reference_passes = 1;
    MapStackReport r1;
    const FrameBuffer once = stackcore::run_map_stack(source, settings, nullptr, r1);

    settings.reference_passes = 2;
    MapStackReport r2;
    const FrameBuffer twice = stackcore::run_map_stack(source, settings, nullptr, r2);

    const double rms1 = rms_vs_truth(once, truth, 24, 4);
    const double rms2 = rms_vs_truth(twice, truth, 24, 4);
    std::printf("           参照1回=%.5f 参照2回=%.5f (%+.1f%%)\n", rms1, rms2,
                100.0 * (rms2 / rms1 - 1.0));

    MT_CHECK_EQ(r1.passes_run, 1);
    MT_CHECK_EQ(r2.passes_run, 2);
    MT_CHECK_EQ(r1.alignment_frame_passes,
                static_cast<long long>(r1.frames_analyzed));
    MT_CHECK_EQ(r2.alignment_frame_passes,
                static_cast<long long>(r2.frames_analyzed) * 2);
    MT_CHECK(r1.consensus_fallback_frames <= r1.alignment_frame_passes);
    MT_CHECK(r2.consensus_fallback_frames <= r2.alignment_frame_passes);
    // 悪化しないことを要求する。改善幅は素材によるので下限は課さない。
    if (!(rms2 <= rms1 * 1.02)) {
        microtest::fail("反復精密化で悪化した: 1回=" + microtest::mt_str(rms1) +
                        " 2回=" + microtest::mt_str(rms2));
    }
}

MT_TEST(sidecar_保存して読み直した解析結果で元と同じ画像が出る) {
    // M3の受け入れ条件「サイドカーからの再スタックが解析済み結果と一致する」。
    // 一致とは目視で同じという意味ではなく、ビット単位で同じという意味。
    const int size = 96;
    const SyntheticSource source(size, 40, 2.0, 1.0, 0.02, 606);

    MapStackSettings settings;
    settings.reference_top_percent = 60.0;
    settings.ap_top_percent = 50.0;
    settings.ap.ap_size = 32;
    settings.local.search_radius = 8;
    settings.reference_passes = 2;

    MapStackReport direct_report;
    const FrameBuffer direct = stackcore::run_map_stack(source, settings, nullptr, direct_report);

    MapStackReport analyze_report;
    stackcore::AnalysisData analysis =
        stackcore::analyze_map_stack(source, settings, nullptr, analyze_report);
    analysis.source_size = 12345;

    const std::string path = "/tmp/lunastack_test_sidecar.lstk";
    stackcore::save_sidecar(path, analysis);

    stackcore::AnalysisData loaded;
    stackcore::load_sidecar(path, loaded);
    std::remove(path.c_str());

    // 入力の同一性チェックが効くこと。
    std::string message;
    MT_CHECK(stackcore::matches_source(loaded, 12345, 40, size, size, 1, message));
    MT_CHECK(!stackcore::matches_source(loaded, 999, 40, size, size, 1, message));
    MT_CHECK(!message.empty());

    MapStackReport restack_report;
    const FrameBuffer restacked =
        stackcore::stack_from_analysis(source, settings, loaded, nullptr, restack_report);

    MT_CHECK_EQ(restack_report.ap_count, direct_report.ap_count);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (direct.row(0, y)[x] != restacked.row(0, y)[x]) {
                microtest::fail("再スタックが元と一致しない (" + microtest::mt_str(x) + "," +
                                microtest::mt_str(y) + ")");
                return;
            }
        }
    }
}

MT_TEST(sidecar_Drizzleを掛けても再スタックが元と一致する) {
    // GUIは「Drizzle倍率の変更は再スタックだけで済む」という前提で動く。
    // 拡大グリッドでは、どのAPからも寄与を受けない画素を参照画像で埋める処理が
    // 入力サイズの参照を拡大出力に当てることになるので、そこが食い違わないかを見る。
    const int size = 96;
    const SyntheticSource source(size, 30, 2.0, 1.0, 0.02, 909);

    MapStackSettings settings;
    settings.reference_top_percent = 60.0;
    settings.ap_top_percent = 50.0;
    settings.ap.ap_size = 32;
    settings.local.search_radius = 8;
    settings.reference_passes = 1;
    settings.drizzle_scale = 2.0;
    settings.pixfrac = 0.9;

    MapStackReport direct_report;
    const FrameBuffer direct = stackcore::run_map_stack(source, settings, nullptr, direct_report);

    MapStackReport analyze_report;
    const stackcore::AnalysisData analysis =
        stackcore::analyze_map_stack(source, settings, nullptr, analyze_report);

    MapStackReport restack_report;
    const FrameBuffer restacked =
        stackcore::stack_from_analysis(source, settings, analysis, nullptr, restack_report);

    MT_CHECK_EQ(direct.width(), size * 2);
    MT_CHECK_EQ(restacked.width(), direct.width());
    MT_CHECK_EQ(restacked.height(), direct.height());
    for (int y = 0; y < direct.height(); ++y) {
        for (int x = 0; x < direct.width(); ++x) {
            if (direct.row(0, y)[x] != restacked.row(0, y)[x]) {
                microtest::fail("Drizzle付きの再スタックが元と一致しない (" +
                                microtest::mt_str(x) + "," + microtest::mt_str(y) + ")");
                return;
            }
        }
    }
}

MT_TEST(map_2パスでもDrizzle倍率が二重に掛からない) {
    // 参照の反復精密化があると、途中のパスの出力が次の参照になる。
    // そこにDrizzleを掛けると倍率が積み上がり、2パス2倍で出力が4倍になる。
    // APの座標は元の参照のままなので、画像の左上1/4にしか中身が入らない。
    // 実データのGUIで、AP枠が左上に縮んで並ぶ形で見つかった。
    const int size = 96;
    const SyntheticSource source(size, 30, 2.0, 1.0, 0.02, 1111);

    MapStackSettings settings;
    settings.reference_top_percent = 60.0;
    settings.ap_top_percent = 50.0;
    settings.ap.ap_size = 32;
    settings.local.search_radius = 8;
    settings.drizzle_scale = 2.0;
    settings.pixfrac = 0.9;

    settings.reference_passes = 1;
    MapStackReport r1;
    const FrameBuffer once = stackcore::run_map_stack(source, settings, nullptr, r1);
    MT_CHECK_EQ(once.width(), size * 2);

    settings.reference_passes = 2;
    MapStackReport r2;
    const FrameBuffer twice = stackcore::run_map_stack(source, settings, nullptr, r2);
    MT_CHECK_EQ(twice.width(), size * 2);
    MT_CHECK_EQ(twice.height(), size * 2);

    // 解析側も同じ。参照は入力と同じ座標系に居なければならない。
    MapStackReport r3;
    const stackcore::AnalysisData analysis =
        stackcore::analyze_map_stack(source, settings, nullptr, r3);
    MT_CHECK_EQ(analysis.width, size);
    MT_CHECK_EQ(analysis.height, size);

    MapStackReport r4;
    const FrameBuffer restacked =
        stackcore::stack_from_analysis(source, settings, analysis, nullptr, r4);
    MT_CHECK_EQ(restacked.width(), size * 2);
    for (int y = 0; y < twice.height(); ++y) {
        for (int x = 0; x < twice.width(); ++x) {
            if (twice.row(0, y)[x] != restacked.row(0, y)[x]) {
                microtest::fail("2パス+Drizzleの再スタックが元と一致しない (" +
                                microtest::mt_str(x) + "," + microtest::mt_str(y) + ")");
                return;
            }
        }
    }
}

MT_TEST(sidecar_選択率だけ変えて解析なしで再スタックできる) {
    // サイドカーの目的そのもの。解析をやり直さずに選択率を変える。
    const int size = 96;
    const SyntheticSource source(size, 40, 2.0, 1.0, 0.02, 707);

    MapStackSettings settings;
    settings.reference_top_percent = 60.0;
    settings.ap_top_percent = 50.0;
    settings.ap.ap_size = 32;
    settings.local.search_radius = 8;
    settings.reference_passes = 1;

    MapStackReport rep;
    const stackcore::AnalysisData analysis =
        stackcore::analyze_map_stack(source, settings, nullptr, rep);

    MapStackSettings other = settings;
    other.ap_top_percent = 20.0;
    MapStackReport rep2;
    const FrameBuffer few =
        stackcore::stack_from_analysis(source, other, analysis, nullptr, rep2);

    MapStackReport rep3;
    const FrameBuffer many =
        stackcore::stack_from_analysis(source, settings, analysis, nullptr, rep3);

    MapStackSettings fixed = settings;
    fixed.ap_top_count = 7;
    MapStackReport rep4;
    const FrameBuffer fixed_count =
        stackcore::stack_from_analysis(source, fixed, analysis, nullptr, rep4);
    (void)fixed_count;
    MT_CHECK_EQ(rep4.frames_per_ap, 7);

    // 採用枚数が変われば結果も変わる（同じ解析を使い回せている証拠）。
    MT_CHECK(rep2.frames_per_ap < rep3.frames_per_ap);
    bool differs = false;
    for (int y = 0; y < size && !differs; ++y) {
        for (int x = 0; x < size; ++x) {
            if (few.row(0, y)[x] != many.row(0, y)[x]) {
                differs = true;
                break;
            }
        }
    }
    MT_CHECK(differs);
}

MT_TEST(sidecar_壊れたファイルや別の入力を拒否する) {
    stackcore::AnalysisData d;
    MT_CHECK_THROWS(stackcore::load_sidecar("/tmp/lunastack_no_such_file.lstk", d));

    const std::string path = "/tmp/lunastack_test_broken.lstk";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    const char junk[] = "これはサイドカーではない";
    std::fwrite(junk, 1, sizeof(junk), f);
    std::fclose(f);
    MT_CHECK_THROWS(stackcore::load_sidecar(path, d));
    std::remove(path.c_str());
}

MT_TEST(sidecar_旧窓合成または旧局所場を持つv2以前は再解析を要求する) {
    const std::string path = "/tmp/lunastack_test_sidecar_v2.lstk";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    const char magic[12] = {'L', 'U', 'N', 'A', 'S', 'T', 'K', 'S', 'I', 'D', 'E', '1'};
    const std::uint32_t old_version = 2;
    std::fwrite(magic, 1, sizeof(magic), f);
    std::fwrite(&old_version, 1, sizeof(old_version), f);
    std::fclose(f);

    stackcore::AnalysisData d;
    MT_CHECK_THROWS(stackcore::load_sidecar(path, d));
    std::remove(path.c_str());
}

MT_TEST(map_同じ入力なら結果はビット単位で同じ) {
    // 決定論性の要件。M2ではAP順とフレーム順の両方を固定する必要がある。
    const int size = 96;
    const SyntheticSource source(size, 30, 2.0, 1.0, 0.02, 31337);

    MapStackSettings settings;
    settings.reference_top_percent = 50.0;
    settings.ap_top_percent = 50.0;
    settings.ap.ap_size = 32;
    settings.local.search_radius = 8;

    MapStackReport r1, r2;
    const FrameBuffer a = stackcore::run_map_stack(source, settings, nullptr, r1);
    const FrameBuffer b = stackcore::run_map_stack(source, settings, nullptr, r2);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (a.row(0, y)[x] != b.row(0, y)[x]) {
                microtest::fail("同じ入力で結果が一致しない (" + microtest::mt_str(x) + "," +
                                microtest::mt_str(y) + ")");
                return;
            }
        }
    }
}

MT_TEST(map_窓合成をAPの組に分けても結果はビット単位で同じ) {
    // フレームを1回だけ読む窓合成は、APの足し込みをメモリの上限で組に分ける。
    // 組の分け方（1組・APごと・低メモリ）が違っても、加算の順序は同じなので結果は一致する。
    const int size = 96;
    const SyntheticSource source(size, 30, 2.0, 1.0, 0.02, 911);
    for (int mode = 0; mode < 3; ++mode) {
        MapStackSettings settings;
        settings.reference_top_percent = 60.0;
        settings.ap_top_percent = 40.0;
        settings.ap.ap_size = 32;
        settings.local.search_radius = 8;
        settings.reference_passes = 1;
        settings.stack_mode = mode == 0 ? stackcore::StackMode::Mean
                                        : (mode == 1 ? stackcore::StackMode::QualityWeighted
                                                     : stackcore::StackMode::SigmaClip);
        settings.drizzle_scale = mode == 2 ? 2.0 : 1.0;
        MapStackReport rep;
        const stackcore::AnalysisData analysis = stackcore::analyze_map_stack(source, settings, nullptr, rep);
        MapStackReport r1, r2, r3;
        const FrameBuffer whole = stackcore::stack_from_analysis(source, settings, analysis, nullptr, r1);
        MapStackSettings per_ap = settings;
        per_ap.stack_budget_bytes = 1;  // APごとに1組
        const FrameBuffer split = stackcore::stack_from_analysis(source, per_ap, analysis, nullptr, r2);
        MapStackSettings low = settings;
        low.low_memory = true;
        const FrameBuffer lowmem = stackcore::stack_from_analysis(source, low, analysis, nullptr, r3);
        MT_CHECK_EQ(whole.width(), split.width());
        bool same = true;
        for (int c = 0; c < whole.channels(); ++c) {
            for (int y = 0; y < whole.height(); ++y) {
                if (std::memcmp(whole.row(c, y), split.row(c, y), sizeof(float) * whole.width()) != 0 ||
                    std::memcmp(whole.row(c, y), lowmem.row(c, y), sizeof(float) * whole.width()) != 0) {
                    same = false;
                }
            }
        }
        MT_CHECK(same);
        MT_CHECK_EQ(r1.stack.contributions, r2.stack.contributions);
    }
}
