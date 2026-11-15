#pragma once

#include <vector>

#include "stackcore/global_stage.hpp"
#include "stackcore/map_pipeline.hpp"
#include "stackcore/sidecar.hpp"
#include "stackcore/video_source.hpp"

namespace stackcore {

// 採用率（APごとに使うフレームの割合）の目安を、実際にスタックして測る。
//
// 多く使えばノイズは減るが、質の悪いフレームが混ざって細部が鈍る。どこが釣り合うかは
// 素材（枚数・明るさ・シーイングのばらつき）で違い、実写では 25% が良い動画も、
// 全部使うほうが良い動画もあった。正解の画像は無いので、次のようにして測る。
//
//   1. アライメント済みのフレームを、番号の順に交互に2組へ分ける（撮影時刻の偏りを作らない）
//   2. 採用率ごとに、2組を別々にスタックする。参照画像・APの配置・変位は解析結果のものを
//      共通に使うので、2枚は同じ座標に乗る
//   3. 2枚のフーリエ環相関（空間周波数の帯ごとの相関）を取る。両方に同じく写る模様（信号）は
//      相関し、ノイズは相関しないので、相関は「再現する細部が雑音に対してどれだけあるか」を表す
//   4. 半分の枚数で測った相関を、全フレームを使ったときの値に換算する（2r / (1 + r)）
//   5. 周波数にわたって平均したものを、その採用率の点数にする
//
// 測るのは信号対雑音であって、見た目の鋭さの好みではない。また、撮像素子の固定パターンは
// 2組に同じく写るので、追尾が良く像がほとんど動かない動画では、多く使う側がやや有利に出る。
// あくまで目安である。
struct AdoptionPoint {
    double percent = 0.0;   // 採用率（%）
    int frames_per_ap = 0;  // 全フレームでスタックするときの、APあたりの採用枚数
    double score = 0.0;     // 再現性の点数（0〜1。大きいほど良い）
    // 換算後の相関が 0.5 を下回る空間周波数（cycle/px）。再現する細部の細かさの目安。
    double cutoff = 0.0;
};

struct AdoptionDiagnosis {
    // 判断に使えるだけのデータが無かった（フレームが少ない・APに覆われた範囲が狭い）。
    bool insufficient = false;
    std::vector<AdoptionPoint> points;  // 採用率の昇順
    double best_percent = 0.0;       // 点数が最大の採用率
    // 目安として勧める採用率。最大とほぼ同じ点数（差が2%以内）の中で、いちばん少ないもの。
    double suggested_percent = 0.0;
    int tile_size = 0;   // 相関を取った升目の大きさ（画素）
    int tiles = 0;       // 使った升目の数
    int frames = 0;      // 測定に使ったフレーム数（2組の合計）
};

// analysis はアライメントの結果（analyze_map_alignment / サイドカー）。
// settings の加算方式・輝度正規化はそのまま使う。ドリズルは掛けない（等倍で比べる）。
// progress が false を返したら Cancelled を投げる。
AdoptionDiagnosis diagnose_adoption(const VideoSource& source, const MapStackSettings& settings,
                                    const AnalysisData& analysis, const ProgressFn& progress);

}  // namespace stackcore
