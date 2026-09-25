#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "stackcore/ap_placer.hpp"
#include "stackcore/global_stage.hpp"
#include "stackcore/local_aligner.hpp"

namespace stackcore {

// 解析パスの結果。これがあれば、解析をやり直さずに
// 選択率などを変えて再スタックできる（仕様書 §5.2）。
//
// 旧型機ほど解析の再利用価値が高い。木星4617フレームの解析に3分かかるが、
// 「上位10%と25%を見比べる」だけのために毎回3分待つのは実用的でない。
struct AnalysisData {
    // 入力の同一性を確かめるための情報。
    // 別のファイルや、中身の変わったファイルに対して
    // 古い解析結果を適用すると、黙って誤った画像が出る。
    std::int64_t source_size = 0;
    int source_frames = 0;
    int width = 0;
    int height = 0;
    int channels = 0;

    // グローバル段の結果（全フレームぶん）。
    std::vector<FrameInfo> frames;
    int reference_index = 0;
    double reference_mean = 0.0;

    // MAP段の結果。
    int ap_size = 0;
    int ap_grid_step = 0;
    std::vector<AlignmentPoint> points;
    // 局所アライメントを掛けたフレームの番号（＝グローバル段で採用されたもの）。
    std::vector<int> analyzed_indices;
    // AP×フレームの変位場。並びは points 順 → analyzed_indices 順。
    std::vector<LocalMatch> matrix;

    // 最終パスの参照画像。
    //
    // これを持たないと再スタックが元と一致しない。窓合成では、どのAPからも
    // 寄与を受けない画素をこの画像で埋めるため、出力の一部そのものになる。
    // また参照の反復精密化を使うと、この画像は前のパスのスタック結果であり、
    // 解析結果だけからは復元できない。
    std::vector<float> reference;  // channels 面ぶん、width*height 連続
};

// サイドカーの保存・読み込み（`.lstk`、独自バイナリ）。
// 失敗時は std::runtime_error を投げる。
void save_sidecar(const std::string& path, const AnalysisData& data);
void load_sidecar(const std::string& path, AnalysisData& data);

// 解析結果が指定の入力に対応するものかを確かめる。
// 一致しなければ理由を message に入れて false を返す。
bool matches_source(const AnalysisData& data, std::int64_t source_size, int frames, int width,
                    int height, int channels, std::string& message);

// 品質評価の結果のキャッシュ（`.lstkq`）。
//
// 品質評価は全フレームを読むので、長い動画では数分かかる。アライメントの前に
// アプリを閉じたり中断したりしても、次に開いたとき品質評価からやり直さずに済むよう
// 結果だけを別ファイルに残す。照合の考え方はサイドカーと同じ。
struct QualityCache {
    std::int64_t source_size = 0;
    int source_frames = 0;
    int width = 0;
    int height = 0;
    int channels = 0;
    GlobalStageReport report;  // frames の index / quality / mean と参照フレーム
};

void save_quality_cache(const std::string& path, const QualityCache& cache);
void load_quality_cache(const std::string& path, QualityCache& cache);

}  // namespace stackcore
