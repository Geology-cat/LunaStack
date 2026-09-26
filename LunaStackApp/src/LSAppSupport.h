#pragma once

// GUI全体で共有する小道具（部品生成・設定表・工程ジョブ）。
//
// **設定の対応表はここにだけ置く。** Drizzle倍率やAPサイズの表を画面の各所に
// 書き写すと、1箇所だけ直し忘れたときに「表示とファイル名と中身が食い違う」
// という気づきにくい壊れ方をする。

#import <Cocoa/Cocoa.h>

#include <memory>
#include <string>
#include <vector>

#include "stackcore/map_pipeline.hpp"
#include "stackcore/metadata.hpp"
#include "stackcore/video_source.hpp"

// 原点が左上のコンテナ。
//
// NSView の座標系は既定で原点が**左下**にある。スクロールビューの
// documentView をそのまま入れると、初期位置が「いちばん下」になり、
// 一覧の先頭ではなく末尾が見えた状態で開いてしまう。
@interface FlippedView : NSView
@end

// 縦にだけスクロールするクリップビュー。
//
// トラックパッドの横スワイプや慣性で、中身が左右へずれないようにする。
// 横の弾性（ラバーバンド）を切るだけでは、中身がわずかでも幅を超えたとき
// （縦スクロールバーを常に表示する設定など）に横へ動いてしまうので、
// 表示範囲の原点 x を常に 0 に固定する。
@interface VerticalClipView : NSClipView
@end

// ウェーブレットのレイヤー数。仕様書 §4.10 の既定。
constexpr int kWaveletLayers = 6;
// Sirilは係数99まで許すが、スライダーは実画像で扱いやすく、過強調しにくい範囲にする。
// 20でも従来上限3の6倍以上あり、木星では16,8,3まで効果を確認している。
constexpr double kWaveletGuiSharpenMaximum = 20.0;

// ウェーブレット欄の「±ボタンと数値欄つき」のつまみの番号。
//   2j = レイヤー j の強調、2j+1 = レイヤー j のノイズ、以下は連動の強さ・輪抑制。
constexpr int kAdjustLinked = kWaveletLayers * 2;
constexpr int kAdjustDering = kWaveletLayers * 2 + 1;
constexpr int kAdjustCount = kWaveletLayers * 2 + 2;

// Drizzle倍率（セグメントの並び順）。
constexpr int kDrizzleChoiceCount = 4;
inline double LSDrizzleScaleAt(NSInteger index) {
    static const double scales[kDrizzleChoiceCount] = {1.0, 1.5, 2.0, 3.0};
    if (index < 0 || index >= kDrizzleChoiceCount) return 1.0;
    return scales[index];
}

// 位置合わせ領域の大きさ（ポップアップの並び順）。0 は自動。
constexpr int kApSizeChoiceCount = 7;
inline int LSApSizeAt(NSInteger index) {
    static const int sizes[kApSizeChoiceCount] = {0, 32, 48, 64, 96, 128, 200};
    if (index < 0 || index >= kApSizeChoiceCount) return 0;
    return sizes[index];
}

enum class OutputFormat { Tiff16, TiffFloat32, FitsFloat32, Png16 };

void write_output_image(const std::string& path, const stackcore::FrameBuffer& image,
                        OutputFormat format,
                        const stackcore::ImageMetadata& metadata = stackcore::ImageMetadata());

NSTextField* MakeLabel(NSString* text);

// 1ファイルぶんの処理。
//
// **GUIから完全に切り離してある。** GUIの各工程も自己検証の一括経路も
// 同じ関数を通るので、「経路によって挙動が違う」という食い違いが起きない。
enum class JobStage { Quality, Alignment, Stack, Full };

struct JobRequest {
    std::string path;
    stackcore::OpenOptions options;
    stackcore::MapStackSettings settings;
    bool global_only = false;
    bool low_memory = false;
    JobStage stage = JobStage::Full;
    // 前工程の結果。工程をまたいで画素を持たず、解析値だけを渡す。
    std::shared_ptr<stackcore::GlobalStageReport> quality;
    std::shared_ptr<stackcore::GlobalStageReport> global;
    std::shared_ptr<stackcore::AnalysisData> analysis;
};

struct JobResult {
    JobStage stage = JobStage::Full;
    std::shared_ptr<stackcore::GlobalStageReport> quality;
    std::shared_ptr<stackcore::GlobalStageReport> global;
    std::shared_ptr<stackcore::AnalysisData> analysis;  // MAPモードのみ
    std::vector<stackcore::FrameInfo> frames;           // 品質グラフ用
    std::shared_ptr<stackcore::FrameBuffer> image;      // スタック工程のとき
    std::shared_ptr<stackcore::MapStackReport> map_report;  // アライメントの内訳（表示用）
    std::vector<int> stacked_frames;  // 加算に使ったフレーム（撮影時刻の計算用）
    int frames_combined = 0;          // 1画素あたりに加算した枚数（FITSのNCOMBINE）
    std::string error;
    bool cancelled = false;
};

JobResult run_job(const JobRequest& req, const stackcore::ProgressFn& progress);

// AP別の平均品質（ヒートマップ用）。行列の並びは points 順 → analyzed_indices 順。
std::vector<double> ap_mean_quality(const stackcore::AnalysisData& analysis);
