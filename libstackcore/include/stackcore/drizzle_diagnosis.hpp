#pragma once

#include "stackcore/frame_buffer.hpp"
#include "stackcore/map_pipeline.hpp"
#include "stackcore/sidecar.hpp"

namespace stackcore {

// ドリズルが効きそうかの「ざっくり」診断（アライメントの結果から、スタックせずに求める）。
//
// ドリズルで細部が増えるのは、次の3つがそろうときである。
//   1. 像が粗い（アンダーサンプリング）: 等倍の画像に、画素で表せる最も細かい周期
//      （ナイキスト周波数）の近くまで信号が残っている
//   2. フレームごとの位置が画素の端数でばらけている: ばらけていないと画素の隙間を埋められない
//   3. 枚数が足りる: 拡大すると1枚の寄与が細かい画素に分かれ、ノイズが目立つ
// それぞれから「ここまでは効きそう」という倍率を見積もり、いちばん小さいものを答えにする。
// 実際に比べる検査（FRC など）ではないので、目安である。
struct DrizzleDiagnosis {
    // 1. 像の細かさ。信号がノイズの2倍まで落ちる周波数 ÷ ナイキスト周波数。
    //    1 未満なら等倍で細部を取りきれている。測れるのは斜めの最も細かい周期（1.4）までで、
    //    その先は外挿し、2 で頭打ちにする（折り返しの先は1枚の画像からは分からない）。
    double cutoff_ratio = 0.0;
    bool cutoff_extrapolated = false;  // 測った範囲で落ちきらず、外挿した
    // ノイズの水準を背景の空から測れた。false（空が写っていない）なら、像の細かい周期の
    // 一部をノイズとみなすので、1 の見積もりは控えめになる。
    bool floor_from_background = false;
    double limit_sampling = 1.0;       // 1 から見た倍率の上限

    // 2. 位置のばらつき。採用したフレームの変位の端数を、画素を 2×2 / 3×3 に分けた升目に数え、
    //    十分な枚数が入った升目の割合（APの中央値、0〜1）。
    double phase_coverage2 = 0.0;
    double phase_coverage3 = 0.0;
    double limit_phase = 1.0;

    // 3. APあたりの採用枚数と、それから見た上限。
    int frames_per_ap = 0;
    double limit_frames = 1.0;

    // 見積もった倍率（1.0 / 1.5 / 2.0）。1.0 なら「効果は小さい」。
    double suggested_scale = 1.0;
    // いちばん厳しかった条件（0=像の細かさ 1=位置のばらつき 2=枚数）。
    int limiting = 0;
    // 判断に使えるだけのデータがなかった（APが無い・参照画像が小さすぎるなど）。
    bool insufficient = false;
};

// settings の ap_top_percent / ap_top_count は、スタックで使うのと同じ選び方で数える。
DrizzleDiagnosis diagnose_drizzle(const AnalysisData& analysis, const FrameBuffer& reference,
                                  const MapStackSettings& settings);

}  // namespace stackcore
