#pragma once

#include <vector>

#include "stackcore/ap_placer.hpp"
#include "stackcore/frame_buffer.hpp"

namespace stackcore {

// AP1点・1フレームぶんの結果。
struct LocalMatch {
    float dx = 0.0f;      // このAPの内容がフレーム内でどれだけ動いたか
    float dy = 0.0f;
    float score = 0.0f;   // ZNCC（信頼度）
    float quality = 0.0f; // AP領域の勾配エネルギー（AP別の選択に使う）
    bool valid = false;   // 相関が信用できるか
};

struct LocalAlignSettings {
    // 探索半径。既定±16px、シーイングに応じ±8〜±32（仕様書 §4.6）。
    int search_radius = 16;

    // ZNCCの下限。これを下回るAPは無効とし、近傍から補間する。
    double min_score = 0.5;

    // ピークが「変位ゼロのとき」よりどれだけ良くなければならないか。
    //
    // 惑星の縞のように一方向にしか模様がない領域では相関面が尾根状になり、
    // ほとんど改善しないままピークが探索範囲の縁まで滑る。
    // 実データ（木星）でこれが起き、AP×フレームの24%が縁に張り付いた。
    // ZNCCの絶対値は0.96と高いままなので min_score では捕まらない。
    // 「動かした甲斐」を見ることで、その手の領域だけを弾ける。
    //
    // ただし既定は0（無効）にしてある。0.02で試したところ、真の変位が小さい
    // 正常なAPまで巻き添えにして無効率が24%から89%へ跳ね上がった。
    // 改善量の絶対値は「真の変位の大きさ」にも依存するので、
    // これ単独をしきい値にするのは筋が悪い。
    // 尾根状の領域は、AP配置側で構造テンソルによって除くほうが確実である
    // （ap_placer の min_structure_ratio）。この値は手動調整用に残す。
    double min_peak_margin = 0.0;

    // 隣接APとの変位差の許容量を、AP間隔に対する比で指定する。
    // 仕様書 §4.6 は「AP間隔の1/4超」を不自然としている。
    double neighbor_clip_ratio = 0.25;
};

// 1フレームぶんの変位場修復で行った処理。
// 診断表示と、相関の回帰を実データで検知するために返す。
struct LocalFieldRepairStats {
    // APごとの変位が空間的に不整合だったため、全APをフレーム共通の
    // サブピクセル変位へ退避させた。
    bool used_global_consensus = false;
    // 一貫した変位場の中に孤立していた相関外れ値の数。
    std::size_t consensus_outliers = 0;
    // 有効AP変位の成分別中央値からの距離の中央値（px）。
    double median_deviation = 0.0;
};

// 1フレームぶんのAP変位場に対して外れ値処理を行う（仕様書 §4.6）。
//
//   1. AP変位場全体の空間的一貫性を検査する
//   2. 不整合な場合はフレーム共通のサブピクセル変位へ安全に退避する
//   3. 一貫している場合は孤立外れ値を除き、無効APを近傍から補間する
//   4. 隣接APとの変位差が大きすぎるものを平滑化制約でクリップする
//
// ap_grid_step はAP中心どうしの間隔（APサイズの1/2）。
// この関数は matches を書き換える。
LocalFieldRepairStats repair_displacement_field(const std::vector<AlignmentPoint>& points,
                                                std::vector<LocalMatch>& matches,
                                                int ap_grid_step,
                                                const LocalAlignSettings& settings);

}  // namespace stackcore
