#include "stackcore/map_pipeline.hpp"

#include <dispatch/dispatch.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "stackcore/quality.hpp"
#include "stackcore/resample.hpp"
#include "stackcore/simple_stacker.hpp"
#include "stackcore/zncc_matcher.hpp"

namespace stackcore {
namespace {

void notify(const ProgressFn& progress, const char* stage, int done, int total) {
    if (progress && !progress(stage, done, total)) throw Cancelled();
}

// APごとの相関は互いに独立しており、各AP専用のZnccMatcherを持つ。
// GCDへAP番号を渡して並列化しても共有の加算順序には触れないため、
// スタック出力の決定論性を保ったまま解析だけを高速化できる。
template <typename Fn>
void parallel_alignment_points(std::size_t count, const Fn& fn) {
    if (count < 4) {
        for (std::size_t i = 0; i < count; ++i) fn(i);
        return;
    }

    struct Context {
        const Fn* function = nullptr;
        std::atomic<bool> failed{false};
        std::mutex failure_mutex;
        std::exception_ptr failure;
    } context;
    context.function = &fn;

    dispatch_apply_f(
        count, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), &context,
        [](void* raw, std::size_t i) {
            Context* c = static_cast<Context*>(raw);
            if (c->failed.load(std::memory_order_relaxed)) return;
            try {
                (*c->function)(i);
            } catch (...) {
                std::lock_guard<std::mutex> lock(c->failure_mutex);
                if (!c->failure) c->failure = std::current_exception();
                c->failed.store(true, std::memory_order_relaxed);
            }
        });

    if (context.failure) std::rethrow_exception(context.failure);
}

// AP領域の勾配エネルギー。AP別のフレーム選択に使う（仕様書 §4.7）。
// 全体の品質評価と同じ指標だが、範囲がAP領域に限られる。
double ap_gradient_energy(const float* blurred, std::size_t stride, int x0, int y0, int size) {
    double sum = 0.0;
    int count = 0;
    for (int y = y0 + 1; y < y0 + size - 1; ++y) {
        const float* prev = blurred + static_cast<std::size_t>(y - 1) * stride;
        const float* cur = blurred + static_cast<std::size_t>(y) * stride;
        const float* next = blurred + static_cast<std::size_t>(y + 1) * stride;
        for (int x = x0 + 1; x < x0 + size - 1; ++x) {
            const double gx = 0.5 * (static_cast<double>(cur[x + 1]) - cur[x - 1]);
            const double gy = 0.5 * (static_cast<double>(next[x]) - prev[x]);
            sum += gx * gx + gy * gy;
            ++count;
        }
    }
    return count > 0 ? sum / count : 0.0;
}

}  // namespace

FrameBuffer build_global_reference(const VideoSource& source, const GlobalStageReport& global,
                                   const std::vector<FrameInfo>& selected, bool raw_cfa,
                                   const ProgressFn& progress, bool normalize_brightness) {
    if (selected.empty()) {
        throw std::runtime_error("参照画像の生成: 使えるフレームがありません");
    }

    FrameBuffer cfa, rgb;
    const FrameBuffer* first = read_prepared_frame(source, selected[0].index, raw_cfa, cfa, rgb);
    SimpleStacker stacker(first->width(), first->height(), first->channels());

    const int total = static_cast<int>(selected.size());
    for (int i = 0; i < total; ++i) {
        const FrameInfo& info = selected[static_cast<std::size_t>(i)];
        const FrameBuffer* f = read_prepared_frame(source, info.index, raw_cfa, cfa, rgb);
        const double gain = normalize_brightness && info.mean > 1e-9
                                ? global.reference_mean / info.mean
                                : 1.0;
        stacker.add(*f, info.dx, info.dy, gain);
        notify(progress, "参照画像の生成", i + 1, total);
    }

    FrameBuffer reference;
    StackStats stats;
    stacker.finish(reference, stats);
    return reference;
}

namespace {

// 1回ぶんのMAP解析（AP配置→局所アライメント）。窓合成は含まない。
void map_analyze_pass(const VideoSource& source, const MapStackSettings& settings,
                      const FrameBuffer& reference, const std::vector<FrameInfo>& analyzed,
                      const ProgressFn& progress, MapStackReport& report, int pass_index,
                      AnalysisData& out);

// 解析結果を使って窓合成する。
FrameBuffer map_stack_pass(const VideoSource& source, const MapStackSettings& settings,
                           const AnalysisData& analysis, const std::vector<FrameInfo>& analyzed,
                           const FrameBuffer& reference, const ProgressFn& progress,
                           MapStackReport& report, int pass_index);

// 途中パス用の設定（Drizzleを外す）。定義は下。
MapStackSettings intermediate_settings(const MapStackSettings& settings);

// FrameBuffer と連続配列の相互変換（サイドカー用）。
void to_flat(const FrameBuffer& src, std::vector<float>& out);
void from_flat(const std::vector<float>& src, int w, int h, int c, FrameBuffer& out);

}  // namespace

FrameBuffer run_map_stack(const VideoSource& source, const MapStackSettings& settings,
                          const ProgressFn& progress, MapStackReport& report) {
    report = MapStackReport{};

    // --- 1. グローバル段 --------------------------------------------------
    report.global = run_global_stage(source, settings.global, settings.raw_cfa, progress);

    const std::vector<FrameInfo> ref_frames =
        select_top_frames(report.global.frames, settings.reference_top_percent);
    if (ref_frames.empty()) {
        throw std::runtime_error("MAPスタック: 追跡に成功したフレームがありません");
    }
    report.reference_frames = static_cast<int>(ref_frames.size());

    // --- 2. 暫定参照画像（仕様書 §4.4） -----------------------------------
    FrameBuffer reference =
        build_global_reference(source, report.global, ref_frames, settings.raw_cfa, progress);

    // 解析対象はグローバル段で採用されたフレームすべて。
    // 事前に間引くと「フレーム157は木星左上だけ採用」という
    // spatial lucky imaging がそのぶん損なわれる。
    std::vector<FrameInfo> analyzed;
    for (std::size_t i = 0; i < report.global.frames.size(); ++i) {
        if (report.global.frames[i].accepted) analyzed.push_back(report.global.frames[i]);
    }
    report.frames_analyzed = static_cast<int>(analyzed.size());

    // --- 3. MAP処理（必要なら参照を差し替えて繰り返す） -------------------
    //
    // 仕様書 §4.4 の「参照の反復精密化」。暫定参照はグローバル整数変位の
    // 平均であり、局所歪みが平均されて残っている。MAPの結果はそれより
    // 局所歪みが取れているので、それを新しい参照にすればテンプレートの質が上がる。
    const int passes = settings.reference_passes < 1 ? 1 : settings.reference_passes;
    FrameBuffer result;
    for (int pass = 0; pass < passes; ++pass) {
        AnalysisData analysis;
        map_analyze_pass(source, settings, reference, analyzed, progress, report, pass, analysis);
        // 途中のパスはDrizzleを掛けない。結果が次の参照になるため。
        const bool last_pass = (pass + 1 == passes);
        const MapStackSettings stack_settings =
            last_pass ? settings : intermediate_settings(settings);
        result = map_stack_pass(source, stack_settings, analysis, analyzed, reference, progress,
                                report, pass);
        report.passes_run = pass + 1;
        if (pass + 1 < passes) {
            // 次のパスの参照にする。中身を持ち替えるだけで再計算はしない。
            reference.reset(result.width(), result.height(), result.channels());
            for (int c = 0; c < result.channels(); ++c) {
                for (int y = 0; y < result.height(); ++y) {
                    const float* s = result.row(c, y);
                    float* d = reference.row(c, y);
                    for (int x = 0; x < result.width(); ++x) d[x] = s[x];
                }
            }
            reference.invalidate_luma();
            reference.set_source_bit_depth(result.source_bit_depth());
        }
    }
    return result;
}

namespace {


void to_flat(const FrameBuffer& src, std::vector<float>& out) {
    const std::size_t pixels = static_cast<std::size_t>(src.width()) * src.height();
    out.assign(pixels * static_cast<std::size_t>(src.channels()), 0.0f);
    for (int c = 0; c < src.channels(); ++c) {
        for (int y = 0; y < src.height(); ++y) {
            const float* s = src.row(c, y);
            float* d = out.data() + static_cast<std::size_t>(c) * pixels +
                       static_cast<std::size_t>(y) * src.width();
            for (int x = 0; x < src.width(); ++x) d[x] = s[x];
        }
    }
}

void from_flat(const std::vector<float>& src, int w, int h, int c, FrameBuffer& out) {
    out.reset(w, h, c);
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    for (int ch = 0; ch < c; ++ch) {
        for (int y = 0; y < h; ++y) {
            const float* s = src.data() + static_cast<std::size_t>(ch) * pixels +
                             static_cast<std::size_t>(y) * w;
            float* d = out.row(ch, y);
            for (int x = 0; x < w; ++x) d[x] = s[x];
        }
    }
    out.invalidate_luma();
}

void map_analyze_pass(const VideoSource& source, const MapStackSettings& settings,
                      const FrameBuffer& reference, const std::vector<FrameInfo>& analyzed,
                      const ProgressFn& progress, MapStackReport& report, int pass_index,
                      AnalysisData& out) {
    // --- AP配置（§4.5） ---------------------------------------------------
    int ap_size = 0;
    const std::vector<AlignmentPoint> points =
        place_alignment_points(reference, settings.ap, ap_size);
    if (points.empty()) {
        throw std::runtime_error(
            "MAPスタック: APが1つも置けませんでした。"
            "--ap-size を小さくするか、--ap-gradient / --ap-level を下げてください");
    }
    report.ap_size = ap_size;
    report.ap_grid_step = ap_size / 2;
    report.ap_count = static_cast<int>(points.size());
    if (pass_index == 0) {
        report.ap_frame_pairs = 0;
        report.invalid_matches = 0;
        report.clipped_matches = 0;
    }

    const int radius = settings.local.search_radius;
    const int search_size = ap_size + 2 * radius;
    const int half = ap_size / 2;

    // テンプレートはAPごとに1回だけ用意する。
    //
    // マッチャをAPごとに持つのは、テンプレートのFFTを毎フレーム計算し直さない
    // ため。1つを使い回すとAP×フレームごとに前方FFTがもう1回増える（1.5倍）。
    // 代償としてAPあたり約256KB（128x128の複素バッファ4枚）を持つ。
    // APが数百になる4K級では100MB規模になるので、M7の性能チューニングで
    // 「テンプレートのFFTだけAP別に持ち、作業バッファは共有する」形に分ける。
    std::vector<std::unique_ptr<ZnccMatcher>> matchers;
    matchers.reserve(points.size());
    std::vector<bool> template_ok(points.size(), false);
    std::vector<float> tmpl(static_cast<std::size_t>(ap_size) * ap_size);

    for (std::size_t a = 0; a < points.size(); ++a) {
        matchers.emplace_back(new ZnccMatcher(ap_size, radius));
        copy_patch_clamped(reference.luma(), reference.width(), reference.height(),
                           reference.stride(), points[a].cx - half, points[a].cy - half,
                           tmpl.data(), ap_size, ap_size, static_cast<std::size_t>(ap_size));
        template_ok[a] =
            matchers[a]->set_template(tmpl.data(), static_cast<std::size_t>(ap_size));
    }

    // AP×フレームの結果。500AP×10000フレームで16バイトなら80MB（仕様書 §4.1）。
    const std::size_t ap_count = points.size();
    const std::size_t frame_count = analyzed.size();
    std::vector<LocalMatch> matrix(ap_count * frame_count);

    FrameBuffer cfa, rgb;
    QualityWorkspace ws;
    std::vector<LocalMatch> per_frame(ap_count);

    const char* align_stage = pass_index == 0 ? "局所アライメント" : "局所アライメント(2回目)";

    for (std::size_t fi = 0; fi < frame_count; ++fi) {
        const FrameInfo& info = analyzed[fi];
        const FrameBuffer* f =
            read_prepared_frame(source, info.index, settings.raw_cfa, cfa, rgb);

        // 品質評価はぼかした輝度に対して行う（仕様書 §4.3 と同じ前処理）。
        ws.ensure(f->stride() * static_cast<std::size_t>(f->height()));
        gaussian_blur_5tap(f->luma(), ws.blurred.data(), f->width(), f->height(), f->stride(),
                           ws.scratch.data());

        parallel_alignment_points(ap_count, [&](std::size_t a) {
            LocalMatch& m = per_frame[a];
            m = LocalMatch{};

            // グローバル変位を打ち消した位置を中心に探索する。
            // ここを忘れるとグローバルな流れぶんが局所変位に化けて
            // 探索半径を食い潰す。
            const int cx = points[a].cx - info.dx;
            const int cy = points[a].cy - info.dy;

            const int qx = std::max(0, std::min(f->width() - ap_size, cx - half));
            const int qy = std::max(0, std::min(f->height() - ap_size, cy - half));
            if (settings.global.quality_metric == QualityMetric::FrequencyBandPowerRatio) {
                thread_local QualityWorkspace frequency_workspace;
                m.quality = static_cast<float>(frequency_band_power_ratio_preblurred(
                    ws.blurred.data() + static_cast<std::size_t>(qy) * f->stride() + qx,
                    ap_size, ap_size, f->stride(), frequency_workspace));
            } else {
                m.quality = static_cast<float>(
                    ap_gradient_energy(ws.blurred.data(), f->stride(), qx, qy, ap_size));
            }

            if (!template_ok[a]) {
                m.valid = false;
                return;
            }

            // GCDワーカーごとの探索バッファ。APごとの確保を避ける。
            thread_local std::vector<float> search;
            const std::size_t needed = static_cast<std::size_t>(search_size) * search_size;
            if (search.size() < needed) search.resize(needed);
            copy_patch_clamped(f->luma(), f->width(), f->height(), f->stride(),
                               cx - half - radius, cy - half - radius, search.data(),
                               search_size, search_size, static_cast<std::size_t>(search_size));
            const MatchResult res =
                matchers[a]->match(search.data(), static_cast<std::size_t>(search_size));

            // 相関が求めたのは「グローバル補正後の位置からのずれ」。
            // 切り出しに使う変位はグローバル変位と足し合わせたものになる。
            m.dx = static_cast<float>(res.dx - info.dx);
            m.dy = static_cast<float>(res.dy - info.dy);
            m.score = static_cast<float>(res.score);
            // 有効と認める条件は3つ。
            //   1. 相関そのものが成立している（ZNCCが下限以上）
            //   2. ピークが探索範囲の縁に張り付いていない
            //   3. ピークが「動かさない場合」より有意に良い
            // 3が無いと、一方向にしか模様がない領域でピークが縁まで滑る。
            m.valid = res.score >= settings.local.min_score && !res.at_search_limit &&
                      (res.score - res.score_at_zero) >= settings.local.min_peak_margin;
        });

        // 共有カウンタは並列領域の外で集計する。
        for (const LocalMatch& match : per_frame) {
            if (!match.valid) ++report.invalid_matches;
        }

        // 外れ値処理（無効APの補間と隣接クリップ）。
        // 補間の基準はグローバル変位ぶんを除いた「局所ずれ」で行いたいので、
        // いったんそちらへ揃えてから処理し、戻す。
        for (std::size_t a = 0; a < ap_count; ++a) {
            per_frame[a].dx += static_cast<float>(info.dx);
            per_frame[a].dy += static_cast<float>(info.dy);
        }
        const std::vector<LocalMatch> before = per_frame;
        repair_displacement_field(points, per_frame, report.ap_grid_step, settings.local);
        for (std::size_t a = 0; a < ap_count; ++a) {
            if (before[a].valid &&
                (before[a].dx != per_frame[a].dx || before[a].dy != per_frame[a].dy)) {
                ++report.clipped_matches;
            }
            per_frame[a].dx -= static_cast<float>(info.dx);
            per_frame[a].dy -= static_cast<float>(info.dy);
            matrix[a * frame_count + fi] = per_frame[a];
        }

        report.ap_frame_pairs += static_cast<long long>(ap_count);
        notify(progress, align_stage, static_cast<int>(fi) + 1, static_cast<int>(frame_count));
    }


    // 解析結果をまとめる。
    out.width = reference.width();
    out.height = reference.height();
    out.channels = reference.channels();
    out.ap_size = ap_size;
    out.ap_grid_step = report.ap_grid_step;
    out.points = points;
    out.analyzed_indices.clear();
    out.analyzed_indices.reserve(analyzed.size());
    for (std::size_t i = 0; i < analyzed.size(); ++i) {
        out.analyzed_indices.push_back(analyzed[i].index);
    }
    out.matrix.swap(matrix);
    to_flat(reference, out.reference);
}

// 途中パス用の設定。
//
// **Drizzleは最終出力のためのものであり、参照画像に掛けてはいけない。**
// 参照は相関のテンプレートであって、入力と同じ座標系に居なければならない。
// 拡大した結果をそのまま次のパスの参照にすると、パスごとに倍率が掛かって
// 2パス2倍なら出力が4倍になり、APの座標だけ元のままなので
// 画像の左上1/4にしか中身が入らない。
MapStackSettings intermediate_settings(const MapStackSettings& settings) {
    MapStackSettings s = settings;
    s.drizzle_scale = 1.0;
    s.pixfrac = 1.0;
    return s;
}

FrameBuffer map_stack_pass(const VideoSource& source, const MapStackSettings& settings,
                           const AnalysisData& analysis, const std::vector<FrameInfo>& analyzed,
                           const FrameBuffer& reference, const ProgressFn& progress,
                           MapStackReport& report, int pass_index) {
    const std::size_t ap_count = analysis.points.size();
    const std::size_t frame_count = analyzed.size();
    const std::vector<LocalMatch>& matrix = analysis.matrix;
    const std::vector<AlignmentPoint>& points = analysis.points;
    const int ap_size = analysis.ap_size;
    FrameBuffer cfa, rgb;
    // --- AP別に上位N%を選んで窓合成（§4.7・§4.8） ------------------------
    int keep = settings.ap_top_count > 0
                   ? settings.ap_top_count
                   : static_cast<int>(frame_count * settings.ap_top_percent / 100.0 + 0.5);
    if (keep < 1) keep = 1;
    if (keep > static_cast<int>(frame_count)) keep = static_cast<int>(frame_count);
    report.frames_per_ap = keep;

    WindowedStacker stacker(reference.width(), reference.height(), reference.channels(),
                            ap_size, settings.drizzle_scale, settings.pixfrac,
                            settings.stack_mode, settings.sigma_clip_threshold);

    // AP順・フレーム順を固定する（決定論性の要件。実装計画書 §4.2）。
    std::vector<std::size_t> order(frame_count);
    for (std::size_t a = 0; a < ap_count; ++a) {
        for (std::size_t i = 0; i < frame_count; ++i) order[i] = i;
        const std::size_t base = a * frame_count;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t l, std::size_t r) {
            const float ql = matrix[base + l].quality;
            const float qr = matrix[base + r].quality;
            if (ql != qr) return ql > qr;
            return analyzed[l].index < analyzed[r].index;
        });
        std::vector<std::size_t> chosen(order.begin(), order.begin() + keep);
        std::sort(chosen.begin(), chosen.end(), [&](std::size_t l, std::size_t r) {
            return analyzed[l].index < analyzed[r].index;
        });

        double quality_sum = 0.0;
        if (settings.stack_mode == StackMode::QualityWeighted) {
            for (std::size_t fi : chosen) {
                quality_sum += std::max(1e-12, static_cast<double>(matrix[base + fi].quality));
            }
        }

        stacker.begin_ap(points[a].cx, points[a].cy);
        for (std::size_t k = 0; k < chosen.size(); ++k) {
            const std::size_t fi = chosen[k];
            const FrameInfo& info = analyzed[fi];
            const LocalMatch& m = matrix[base + fi];
            const FrameBuffer* f =
                read_prepared_frame(source, info.index, settings.raw_cfa, cfa, rgb);
            const double gain = settings.normalize_brightness && info.mean > 1e-9
                                    ? report.global.reference_mean / info.mean
                                    : 1.0;
            double sample_weight = 1.0;
            if (settings.stack_mode == StackMode::QualityWeighted && quality_sum > 0.0) {
                sample_weight =
                    std::max(1e-12, static_cast<double>(m.quality)) * chosen.size() / quality_sum;
            }
            stacker.add_frame(*f, m.dx, m.dy, gain, sample_weight);
        }
        stacker.end_ap();
        notify(progress, pass_index == 0 ? "窓合成スタック" : "窓合成スタック(2回目)",
               static_cast<int>(a) + 1, static_cast<int>(ap_count));
    }

    FrameBuffer out;
    stacker.finish(out, report.stack, &reference);
    out.set_source_bit_depth(reference.source_bit_depth());
    return out;
}

}  // namespace

namespace {

// グローバル段から、解析対象フレームと暫定参照までを整える共通部分。
struct StageSetup {
    FrameBuffer reference;
    std::vector<FrameInfo> analyzed;
};

StageSetup prepare_stages(const VideoSource& source, const MapStackSettings& settings,
                          const ProgressFn& progress, MapStackReport& report) {
    report.global = run_global_stage(source, settings.global, settings.raw_cfa, progress);

    const std::vector<FrameInfo> ref_frames =
        select_top_frames(report.global.frames, settings.reference_top_percent);
    if (ref_frames.empty()) {
        throw std::runtime_error("MAPスタック: 追跡に成功したフレームがありません");
    }
    report.reference_frames = static_cast<int>(ref_frames.size());

    StageSetup out;
    out.reference =
        build_global_reference(source, report.global, ref_frames, settings.raw_cfa, progress);
    for (std::size_t i = 0; i < report.global.frames.size(); ++i) {
        if (report.global.frames[i].accepted) out.analyzed.push_back(report.global.frames[i]);
    }
    report.frames_analyzed = static_cast<int>(out.analyzed.size());
    return out;
}

}  // namespace

AnalysisData analyze_map_stack(const VideoSource& source, const MapStackSettings& settings,
                               const ProgressFn& progress, MapStackReport& report) {
    report = MapStackReport{};
    StageSetup setup = prepare_stages(source, settings, progress, report);

    // 参照の反復精密化がある場合、最後のパスの解析だけが必要になる。
    // 途中のパスは「次の参照を作るため」に加算まで行う。
    const int passes = settings.reference_passes < 1 ? 1 : settings.reference_passes;
    AnalysisData analysis;
    for (int pass = 0; pass < passes; ++pass) {
        map_analyze_pass(source, settings, setup.reference, setup.analyzed, progress, report,
                         pass, analysis);
        report.passes_run = pass + 1;
        if (pass + 1 < passes) {
            // ここでの加算は次の参照を作るためだけなので、Drizzleは掛けない。
            const MapStackSettings ref_settings = intermediate_settings(settings);
            const FrameBuffer stacked =
                map_stack_pass(source, ref_settings, analysis, setup.analyzed, setup.reference,
                               progress, report, pass);
            setup.reference.reset(stacked.width(), stacked.height(), stacked.channels());
            for (int c = 0; c < stacked.channels(); ++c) {
                for (int y = 0; y < stacked.height(); ++y) {
                    const float* src = stacked.row(c, y);
                    float* dst = setup.reference.row(c, y);
                    for (int x = 0; x < stacked.width(); ++x) dst[x] = src[x];
                }
            }
            setup.reference.invalidate_luma();
            setup.reference.set_source_bit_depth(stacked.source_bit_depth());
        }
    }

    analysis.frames = report.global.frames;
    analysis.reference_index = report.global.reference_index;
    analysis.reference_mean = report.global.reference_mean;
    analysis.source_frames = source.frame_count();
    return analysis;
}

FrameBuffer stack_from_analysis(const VideoSource& source, const MapStackSettings& settings,
                                const AnalysisData& analysis, const ProgressFn& progress,
                                MapStackReport& report) {
    if (analysis.points.empty() || analysis.analyzed_indices.empty()) {
        throw std::runtime_error("再スタック: 解析結果にAPまたはフレームがありません");
    }
    if (analysis.reference.empty()) {
        throw std::runtime_error("再スタック: 解析結果に参照画像が入っていません");
    }

    // 解析時に使った参照画像を復元する。
    // 窓合成では、どのAPからも寄与を受けない画素をこの画像で埋めるので、
    // これが無いと出力が解析済みの結果と一致しない。
    FrameBuffer reference;
    from_flat(analysis.reference, analysis.width, analysis.height, analysis.channels, reference);

    // 解析時のフレーム情報を復元する。
    std::vector<FrameInfo> analyzed;
    analyzed.reserve(analysis.analyzed_indices.size());
    for (std::size_t i = 0; i < analysis.analyzed_indices.size(); ++i) {
        const int idx = analysis.analyzed_indices[i];
        if (idx < 0 || idx >= static_cast<int>(analysis.frames.size())) {
            throw std::runtime_error("再スタック: 解析結果のフレーム番号が範囲外です");
        }
        analyzed.push_back(analysis.frames[static_cast<std::size_t>(idx)]);
    }

    report.global.frames = analysis.frames;
    report.global.reference_index = analysis.reference_index;
    report.global.reference_mean = analysis.reference_mean;
    report.ap_size = analysis.ap_size;
    report.ap_grid_step = analysis.ap_grid_step;
    report.ap_count = static_cast<int>(analysis.points.size());
    report.frames_analyzed = static_cast<int>(analyzed.size());
    report.passes_run = 0;  // 解析は再実行していない

    return map_stack_pass(source, settings, analysis, analyzed, reference, progress, report, 0);
}

}  // namespace stackcore
