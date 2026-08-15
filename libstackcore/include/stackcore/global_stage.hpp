#pragma once

#include <exception>
#include <functional>
#include <vector>

#include "stackcore/global_aligner.hpp"
#include "stackcore/quality.hpp"
#include "stackcore/video_source.hpp"

namespace stackcore {

// フレーム1枚ぶんの解析結果。パス間で保持するのはこれだけで、
// 画素は保持しない（仕様書 §4.1 のストリーミング2パス設計）。
struct FrameInfo {
    int index = 0;
    double quality = 0.0;     // 勾配エネルギー
    double mean = 0.0;        // 平均輝度（輝度正規化に使う）
    int dx = 0;               // グローバル変位
    int dy = 0;
    double similarity = 0.0;  // 参照とのZNCC
    bool accepted = false;
    RejectReason reason = RejectReason::None;
};

struct GlobalStageSettings {
    GlobalAlignSettings align;
    QualityMetric quality_metric = QualityMetric::GradientEnergy;
    // 類似度の外れ値判定の厳しさ（中央値からMAD換算で何σ）。
    double outlier_k = 6.0;
    // 先頭Nフレームだけ処理する（0で全部）。
    int limit = 0;
};

struct GlobalStageReport {
    std::vector<FrameInfo> frames;
    int reference_index = 0;
    AlignMode mode = AlignMode::Lunar;
    int max_shift = 0;
    int rejected_low_similarity = 0;
    int rejected_shift = 0;
    int rejected_outlier = 0;
    double similarity_median = 0.0;
    double similarity_threshold = -1.0;
    double reference_mean = 0.0;

    int accepted_count() const {
        int n = 0;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            if (frames[i].accepted) ++n;
        }
        return n;
    }
};

// 進捗通知。stage は "品質評価" などの表示名。
//
// **false を返すと処理を中断する。** GUIでは数分かかる処理を止められないと
// 使い物にならないので、進捗通知の戻り値をそのまま中断の合図にしている。
// 別途キャンセル用のオブジェクトを持ち回るより、通知の頻度と
// 中断を確認する頻度が自動的に一致するので取りこぼしがない。
using ProgressFn = std::function<bool(const char* stage, int done, int total)>;

// 中断されたときに投げる。呼び出し側が「異常終了」と「利用者による中断」を
// 区別できるよう、専用の型にしている。
class Cancelled : public std::exception {
public:
    const char* what() const noexcept override { return "処理が中断されました"; }
};

// 全フレームの品質と平均輝度だけを評価する（仕様書 §4.3）。
//
// この段階ではdx/dy/similarityは0のまま、全フレームをaccepted=trueにする。
// GUIで品質グラフを確認してから、run_global_alignmentを別に実行できる。
GlobalStageReport evaluate_frame_quality(const VideoSource& source,
                                         const GlobalStageSettings& settings, bool raw_cfa,
                                         const ProgressFn& progress);

// 品質評価済みの結果を使って、グローバルアライメントだけを行う（仕様書 §4.2）。
// 品質の再計算はしない。参照フレームはqualityで選ばれた中央値品質のフレーム。
GlobalStageReport run_global_alignment(const VideoSource& source,
                                       const GlobalStageSettings& settings, bool raw_cfa,
                                       const GlobalStageReport& quality,
                                       const ProgressFn& progress);

// 品質評価とグローバルアライメントを続けて行う互換API（仕様書 §4.2・§4.3）。
//
// 参照フレームは**品質の中央値**のフレームを選ぶ。
// 最高品質を選んではいけない。勾配エネルギーは「シャープさ」と
// 「引き裂かれた段差」を区別できず、テアリングを起こした壊れフレームが
// 最高スコアを取るためである（実装計画書 §6.6）。
//
// raw_cfa が false なら、Bayer入力は読み込み時にデバイヤーする。
GlobalStageReport run_global_stage(const VideoSource& source,
                                   const GlobalStageSettings& settings, bool raw_cfa,
                                   const ProgressFn& progress);

// 採用フレームを品質降順に並べ、上位 percent% を選ぶ。
// 返り値はフレーム番号昇順（加算順を固定するため。決定論性の要件）。
std::vector<FrameInfo> select_top_frames(const std::vector<FrameInfo>& frames, double percent);

// SER/AVIから1フレーム読み、必要ならデバイヤーする。
// 結果が入っているバッファ（cfa か rgb のどちらか）を返す。
const FrameBuffer* read_prepared_frame(const VideoSource& source, int index, bool raw_cfa,
                                       FrameBuffer& cfa, FrameBuffer& rgb);

}  // namespace stackcore
