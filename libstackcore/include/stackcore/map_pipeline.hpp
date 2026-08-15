#pragma once

#include <vector>

#include "stackcore/ap_placer.hpp"
#include "stackcore/global_stage.hpp"
#include "stackcore/local_aligner.hpp"
#include "stackcore/sidecar.hpp"
#include "stackcore/windowed_stacker.hpp"

namespace stackcore {

struct MapStackSettings {
    GlobalStageSettings global;
    // 参照画像を作るのに使うフレームの割合（仕様書 §4.4、既定25%）。
    double reference_top_percent = 25.0;

    ApPlacementSettings ap;
    LocalAlignSettings local;
    // AP別に採用するフレームの割合（仕様書 §4.7、既定10%）。
    double ap_top_percent = 10.0;
    // 1以上なら割合ではなく枚数を直接指定する。0で割合指定。
    int ap_top_count = 0;

    // 加算前にフレーム平均輝度を参照へ揃える。薄雲や透明度変動への対策。
    bool normalize_brightness = true;
    StackMode stack_mode = StackMode::Mean;
    double sigma_clip_threshold = 2.0;

    // 参照の反復精密化（仕様書 §4.4、既定ON）。
    // 1 = 精密化なし（暫定参照だけで1回通す）
    // 2 = MAPの結果を新しい参照にしてもう1回通す
    //
    // 局所アライメントとスタックをもう一度やり直すので、この回数に比例して時間が延びる。
    int reference_passes = 2;

    // Drizzle（仕様書 §4.9）。倍率1.0で無効。
    //
    // 窓合成の枠組みをそのまま使い、出力グリッドを拡大して
    // 各入力画素をAP別のサブピクセル変位込みで投影する。
    // グローバル段の変位は整数なので、Drizzleが意味を持つのは
    // **AP別の局所変位が小数で散っているから**である。
    // 実データ（木星）ではその小数部がほぼ一様に散っていることを確認済み
    // （平均0.493 / 標準偏差0.300、一様分布の理論値は0.500 / 0.289）。
    double drizzle_scale = 1.0;
    double pixfrac = 0.9;

    bool raw_cfa = false;
};

struct MapStackReport {
    GlobalStageReport global;
    int ap_size = 0;
    int ap_grid_step = 0;
    int ap_count = 0;
    int reference_frames = 0;   // 参照画像を作るのに使った枚数
    int frames_analyzed = 0;    // ローカルアライメントを掛けたフレーム数
    long long ap_frame_pairs = 0;
    long long invalid_matches = 0;   // 信頼度不足・縁張り付きで補間したAP×フレーム
    long long clipped_matches = 0;   // 近傍との差でクリップされたAP×フレーム
    int frames_per_ap = 0;
    int passes_run = 0;
    WindowedStackStats stack;
};

// MAP局所アライメント＋オーバーラップ窓合成の全体（仕様書 §4.4〜§4.8）。
//
// 流れ:
//   1. グローバル段（品質評価＋グローバルアライメント）
//   2. 上位N%のグローバルスタックで暫定参照画像を作る（§4.4）
//      = M1の出力そのもの。参照生成に別の実装を持たない。
//   3. 参照画像にAPを配置する（§4.5）
//   4. 全フレーム×全APでZNCC相関＋サブピクセル推定、外れ値処理（§4.6）
//   5. AP別に品質上位N%を選ぶ（§4.7）
//   6. Hann窓オーバーラップ合成（§4.8）
//
// **注意（M2の限界）**: APは参照画像（＝整数変位のグローバルスタック）の上に置き、
// テンプレートもそこから切り出す。参照は個々のフレームよりも柔らかいので、
// 相関のサブピクセル推定はわずかに中心寄りに偏る。
// これは仕様どおりであり（§4.4の「参照の反復精密化」とM3の2パス目が
// まさにこれに対処するために存在する）、M2で測る精度の値を
// アルゴリズムの上限として読まないこと。
FrameBuffer run_map_stack(const VideoSource& source, const MapStackSettings& settings,
                          const ProgressFn& progress, MapStackReport& report);

// 解析だけを行う（仕様書 §5.2 のサイドカー用）。
// 参照の反復精密化がある場合は、最後のパスぶんの解析結果を返す。
// AP変位場と最終パスの参照画像が入るので、これだけで再スタックできる。
AnalysisData analyze_map_stack(const VideoSource& source, const MapStackSettings& settings,
                               const ProgressFn& progress, MapStackReport& report);

// 品質評価・グローバルアライメント済みの結果から、MAP局所アライメントだけを行う。
// 品質評価を再実行しないため、GUIで工程を段階的に進められる。
AnalysisData analyze_map_alignment(const VideoSource& source,
                                   const MapStackSettings& settings,
                                   const GlobalStageReport& global,
                                   const ProgressFn& progress, MapStackReport& report);

// 解析結果から加算だけを行う。
// 選択率（ap_top_percent）を変えて何度も呼べる。
FrameBuffer stack_from_analysis(const VideoSource& source, const MapStackSettings& settings,
                                const AnalysisData& analysis, const ProgressFn& progress,
                                MapStackReport& report);

// 参照画像（グローバルアライメントのみの単純平均スタック）を作る。
// M1の `stack` コマンドと同じ処理。
FrameBuffer build_global_reference(const VideoSource& source, const GlobalStageReport& global,
                                   const std::vector<FrameInfo>& selected, bool raw_cfa,
                                   const ProgressFn& progress,
                                   bool normalize_brightness = true);

}  // namespace stackcore
