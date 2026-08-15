#pragma once

#include <vector>

#include "stackcore/frame_buffer.hpp"

namespace stackcore {

// Alignment Point（局所アライメントの単位）。
struct AlignmentPoint {
    int cx = 0;  // 中心座標（参照フレームの座標系、整数）
    int cy = 0;
    double mean_gradient = 0.0;  // 採否判定に使った平均勾配
    double mean_level = 0.0;     // 採否判定に使った平均輝度
    double min_eigenvalue = 0.0; // 構造テンソルの最小固有値（2次元性の強さ）
};

struct ApPlacementSettings {
    // APサイズ。仕様書 §4.5 の選択肢は 32/48/64/96/128/200。
    // 0 で対象サイズからの自動提案。
    int ap_size = 0;

    // 平均勾配の閾値。画像全体の平均勾配に対する比で指定する。
    // 絶対値で持つと露出やビット深度で意味が変わるため。
    double gradient_ratio = 0.6;

    // 平均輝度の閾値。フレームの最大輝度に対する比。
    // 宇宙空間や月の限界外を除外する。惑星のリムは勾配が強く出るが、
    // 領域の半分が背景なので平均輝度で落ちる。
    double level_ratio = 0.15;

    // 2次元的な構造の強さの下限（構造テンソルの最小固有値）。
    // 画像全体の平均に対する比で指定する。
    //
    // 平均勾配だけでは足りない。惑星の縞のように**一方向にしか**模様がない
    // 領域は平均勾配が高く出るが、縞に沿った方向には手がかりがないため
    // 局所アライメントが定まらない（アパーチャ問題）。
    // 実データ（木星）では、この条件が無いとAP×フレームの24%で
    // 相関ピークが探索範囲の縁まで滑った。ZNCCは0.96と高いままなので
    // 相関値のしきい値では捕まらない。
    //
    // 構造テンソル [[Σgx², Σgxgy],[Σgxgy, Σgy²]] の最小固有値は
    // 「最も手がかりの乏しい方向にどれだけ手がかりがあるか」を表す。
    // Shi-Tomasi の良特徴点判定と同じ考え方。
    double min_structure_ratio = 0.5;

    // AP領域が画像からはみ出すのを許すか。
    // 端のAPは参照できる画素が減り、相関の信頼度が落ちる。
    bool allow_partial_edge = false;

    // 手動配置を使うか（UI設計書 §5.2 の「クリックでAP追加、Deleteで削除」）。
    //
    // **manual_points が空かどうかで判断してはいけない。**
    // 「すべて消去」した状態と「自動配置」を区別できなくなる。
    // 消したのに勝手に置き直されるのは、利用者から見て操作が効いていないのと同じ。
    // 消した結果APが0個なら、黙って自動配置に戻さずエラーにする。
    bool use_manual_points = false;

    // 手動で指定したAP中心。use_manual_points が true のときだけ使う。
    //
    // **閾値による足切りは掛けない。** 利用者が明示的に置いた点を
    // 「勾配が足りない」と黙って捨てると、消えた理由が画面から分からない。
    // 画像からはみ出す点だけは、テンプレートを切り出せないので落とす。
    std::vector<AlignmentPoint> manual_points;
};

// 対象サイズからAPサイズを提案する。
// 明るい領域の短辺の 1/6 程度を目安に、仕様書 §4.5 の選択肢から選ぶ。
int suggest_ap_size(const FrameBuffer& reference);

// 参照フレームにAPを敷き詰める（仕様書 §4.5）。
//
// 配置間隔はAPサイズの1/2（50%オーバーラップ）。これは⑧の継ぎ目防止の前提条件
// であって、単なる密度の調整ではない。
//
// 返り値はAP中心の一覧。順序は y 昇順→x 昇順で固定する（決定論性の要件）。
std::vector<AlignmentPoint> place_alignment_points(const FrameBuffer& reference,
                                                   const ApPlacementSettings& settings,
                                                   int& used_ap_size);

}  // namespace stackcore
