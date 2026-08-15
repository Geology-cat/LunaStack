#include "stackcore/global_stage.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "stackcore/debayer.hpp"
#include "stackcore/frame_selector.hpp"
#include "stackcore/quality.hpp"

namespace stackcore {
namespace {

void notify(const ProgressFn& progress, const char* stage, int done, int total) {
    if (progress && !progress(stage, done, total)) throw Cancelled();
}

int analysis_worker_count(const VideoSource& source, int total) {
    if (!source.supports_concurrent_reads() || total < 8) return 1;
    const unsigned reported = std::thread::hardware_concurrency();
    // FFT作業領域をワーカーごとに持つため、論理CPU数を無制限には使わない。
    // 8コア機では物理コア相当まで使い、旧型2コア機では自動的に2となる。
    const unsigned bounded = std::max(1u, std::min(8u, reported == 0 ? 1u : reported));
    return std::min(total, static_cast<int>(bounded));
}

// フレーム単位の決定論的な並列実行。
// 結果は呼び出し側がフレーム番号の位置へ直接書くため、実行順が変わっても
// 後段のソート・加算順は変わらない。進捗コールバックだけは直列化する。
template <typename Fn>
void parallel_frames(int total, int workers, const char* stage, const ProgressFn& progress,
                     const Fn& fn) {
    if (workers <= 1) {
        for (int i = 0; i < total; ++i) {
            fn(0, i);
            notify(progress, stage, i + 1, total);
        }
        return;
    }

    std::atomic<int> next(0);
    std::atomic<bool> stop(false);
    std::atomic<bool> cancelled(false);
    std::mutex progress_mutex;
    std::mutex failure_mutex;
    std::exception_ptr failure;
    int completed = 0;

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(workers));
    for (int worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&, worker]() {
            try {
                while (!stop.load(std::memory_order_relaxed)) {
                    const int i = next.fetch_add(1, std::memory_order_relaxed);
                    if (i >= total) break;
                    fn(worker, i);

                    std::lock_guard<std::mutex> lock(progress_mutex);
                    ++completed;
                    if (progress && !progress(stage, completed, total)) {
                        cancelled.store(true, std::memory_order_relaxed);
                        stop.store(true, std::memory_order_relaxed);
                    }
                }
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(failure_mutex);
                    if (!failure) failure = std::current_exception();
                }
                stop.store(true, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& thread : threads) thread.join();

    if (failure) std::rethrow_exception(failure);
    if (cancelled.load(std::memory_order_relaxed)) throw Cancelled();
}

}  // namespace

const FrameBuffer* read_prepared_frame(const VideoSource& source, int index, bool raw_cfa,
                                       FrameBuffer& cfa, FrameBuffer& rgb) {
    source.read_frame(index, cfa);
    const SerColorId color = source.color_id();
    if (!raw_cfa && is_bayer(color) && is_supported_bayer(color)) {
        debayer_bilinear(cfa, color, rgb);
        return &rgb;
    }
    return &cfa;
}

GlobalStageReport run_global_stage(const VideoSource& source,
                                   const GlobalStageSettings& settings, bool raw_cfa,
                                   const ProgressFn& progress) {
    const int total = settings.limit > 0 && settings.limit < source.frame_count()
                          ? settings.limit
                          : source.frame_count();

    GlobalStageReport report;
    report.frames.resize(static_cast<std::size_t>(total));

    const int workers = analysis_worker_count(source, total);

    struct QualityWorker {
        FrameBuffer cfa;
        FrameBuffer rgb;
        QualityWorkspace workspace;
    };
    std::vector<std::unique_ptr<QualityWorker>> quality_workers;
    quality_workers.reserve(static_cast<std::size_t>(workers));
    for (int i = 0; i < workers; ++i) {
        quality_workers.emplace_back(new QualityWorker());
    }

    // --- パス1: 全フレームの品質と平均輝度 --------------------------------
    parallel_frames(total, workers, "品質評価", progress, [&](int worker, int i) {
        QualityWorker& w = *quality_workers[static_cast<std::size_t>(worker)];
        const FrameBuffer* f = read_prepared_frame(source, i, raw_cfa, w.cfa, w.rgb);
        FrameInfo& info = report.frames[static_cast<std::size_t>(i)];
        info.index = i;
        info.quality = quality_score(*f, settings.quality_metric, w.workspace);
        info.mean = mean_luma(*f);
    });

    // 参照は品質の中央値のフレーム（理由はヘッダのコメント参照）。
    std::vector<double> sorted;
    sorted.reserve(report.frames.size());
    for (std::size_t i = 0; i < report.frames.size(); ++i) {
        sorted.push_back(report.frames[i].quality);
    }
    std::sort(sorted.begin(), sorted.end());
    const double median_quality = sorted[sorted.size() / 2];

    double best_distance = -1.0;
    for (int i = 0; i < total; ++i) {
        const double d = std::fabs(report.frames[static_cast<std::size_t>(i)].quality -
                                   median_quality);
        if (best_distance < 0.0 || d < best_distance) {
            best_distance = d;
            report.reference_index = i;
        }
    }
    report.reference_mean = report.frames[static_cast<std::size_t>(report.reference_index)].mean;

    // --- パス2: グローバルアライメント ------------------------------------
    FrameBuffer ref_cfa, ref_rgb;
    const FrameBuffer* reference =
        read_prepared_frame(source, report.reference_index, raw_cfa, ref_cfa, ref_rgb);

    struct AlignWorker {
        FrameBuffer cfa;
        FrameBuffer rgb;
        std::unique_ptr<GlobalAligner> aligner;
    };
    std::vector<std::unique_ptr<AlignWorker>> align_workers;
    align_workers.reserve(static_cast<std::size_t>(workers));
    for (int i = 0; i < workers; ++i) {
        std::unique_ptr<AlignWorker> w(new AlignWorker());
        w->aligner.reset(new GlobalAligner(*reference, settings.align));
        align_workers.emplace_back(std::move(w));
    }
    report.mode = align_workers[0]->aligner->mode();
    report.max_shift = align_workers[0]->aligner->max_shift();

    parallel_frames(total, workers, "アライメント", progress, [&](int worker, int i) {
        AlignWorker& w = *align_workers[static_cast<std::size_t>(worker)];
        const FrameBuffer* f = read_prepared_frame(source, i, raw_cfa, w.cfa, w.rgb);
        const GlobalAlignResult r = w.aligner->align(*f);
        FrameInfo& info = report.frames[static_cast<std::size_t>(i)];
        info.dx = r.dx;
        info.dy = r.dy;
        info.similarity = r.similarity;
        info.accepted = r.accepted;
        info.reason = r.reason;
    });

    // 集計は並列領域の外で行い、共有カウンタへの競合を避ける。
    for (const FrameInfo& info : report.frames) {
        if (!info.accepted) {
            if (info.reason == RejectReason::LowCorrelation) ++report.rejected_low_similarity;
            else ++report.rejected_shift;
        }
    }

    // --- 構造的な外れ値の除外 ---------------------------------------------
    std::vector<double> sims;
    sims.reserve(report.frames.size());
    for (std::size_t i = 0; i < report.frames.size(); ++i) {
        if (report.frames[i].accepted) sims.push_back(report.frames[i].similarity);
    }
    const double threshold = similarity_outlier_threshold(sims, settings.outlier_k);
    report.similarity_threshold = threshold;
    if (!sims.empty()) {
        std::vector<double> tmp = sims;
        std::sort(tmp.begin(), tmp.end());
        report.similarity_median = tmp[tmp.size() / 2];
    }
    if (threshold >= 0.0) {
        for (std::size_t i = 0; i < report.frames.size(); ++i) {
            if (report.frames[i].accepted && report.frames[i].similarity < threshold) {
                report.frames[i].accepted = false;
                report.frames[i].reason = RejectReason::StructuralOutlier;
                ++report.rejected_outlier;
            }
        }
    }
    return report;
}

std::vector<FrameInfo> select_top_frames(const std::vector<FrameInfo>& frames, double percent) {
    std::vector<FrameInfo> usable;
    usable.reserve(frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].accepted) usable.push_back(frames[i]);
    }
    if (usable.empty()) return usable;

    // 品質降順。同点はフレーム番号昇順で決める（決定論性の要件）。
    std::sort(usable.begin(), usable.end(), [](const FrameInfo& a, const FrameInfo& b) {
        if (a.quality != b.quality) return a.quality > b.quality;
        return a.index < b.index;
    });

    int keep = static_cast<int>(usable.size() * percent / 100.0 + 0.5);
    if (keep < 1) keep = 1;
    if (keep > static_cast<int>(usable.size())) keep = static_cast<int>(usable.size());
    usable.resize(static_cast<std::size_t>(keep));

    // 加算はフレーム番号の昇順に固定する。
    std::sort(usable.begin(), usable.end(),
              [](const FrameInfo& a, const FrameInfo& b) { return a.index < b.index; });
    return usable;
}

}  // namespace stackcore
