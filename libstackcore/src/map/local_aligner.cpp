#include "stackcore/local_aligner.hpp"

#include <cmath>
#include <stdexcept>

namespace stackcore {
namespace {

// 近傍とみなす距離（AP間隔の倍数）。
// 50%オーバーラップ格子なので、間隔の1.5倍までを取れば
// 上下左右と斜めの8近傍が入る。
constexpr double kNeighborRange = 1.5;

double distance_sq(const AlignmentPoint& a, const AlignmentPoint& b) {
    const double dx = static_cast<double>(a.cx) - b.cx;
    const double dy = static_cast<double>(a.cy) - b.cy;
    return dx * dx + dy * dy;
}

}  // namespace

void repair_displacement_field(const std::vector<AlignmentPoint>& points,
                               std::vector<LocalMatch>& matches, int ap_grid_step,
                               const LocalAlignSettings& settings) {
    if (points.size() != matches.size()) {
        throw std::invalid_argument("外れ値処理: AP数と結果数が一致しません");
    }
    if (points.empty()) return;

    const double range_sq =
        (kNeighborRange * ap_grid_step) * (kNeighborRange * ap_grid_step);

    // --- 1. 無効なAPを近傍の有効APから補間する ----------------------------
    //
    // 無効なAPをそのまま使うと、そのAP領域だけ全く違う場所を切り出して加算し、
    // 出力に局所的な二重像が出る。近傍から埋めるほうが、
    // 「その領域だけシーイングの推定を諦めてグローバル変位に近づける」ことになり実害が小さい。
    //
    // 補間の入力は必ず**元の有効APだけ**にする。補間で埋めた値を次の補間の
    // 入力にすると、無効APが連なった領域で値が芋づる式に広がってしまう。
    const std::vector<LocalMatch> original = matches;

    for (std::size_t i = 0; i < points.size(); ++i) {
        if (original[i].valid) continue;

        double wsum = 0.0, sx = 0.0, sy = 0.0;
        for (std::size_t j = 0; j < points.size(); ++j) {
            if (i == j || !original[j].valid) continue;
            const double d2 = distance_sq(points[i], points[j]);
            if (d2 > range_sq) continue;
            // 距離の逆数で重み付け。近いAPほど強く効かせる。
            const double w = 1.0 / (std::sqrt(d2) + 1.0);
            wsum += w;
            sx += w * original[j].dx;
            sy += w * original[j].dy;
        }

        if (wsum > 0.0) {
            matches[i].dx = static_cast<float>(sx / wsum);
            matches[i].dy = static_cast<float>(sy / wsum);
        } else {
            // 近傍に有効APが1つもない。グローバル変位のまま（0）にしておく。
            matches[i].dx = 0.0f;
            matches[i].dy = 0.0f;
        }
        // valid は false のまま残す。補間で埋めた値であることを
        // 上流（スタック時の重み付けや診断）が知れるようにするため。
    }

    // --- 2. 隣接APとの差が大きすぎる変位をクリップする --------------------
    //
    // シーイングによる歪みは連続的なので、隣り合うAPの変位が大きく違うのは
    // 相関の誤りである可能性が高い。仕様書 §4.6 は「AP間隔の1/4超」を目安としている。
    const double limit = settings.neighbor_clip_ratio * ap_grid_step;
    const std::vector<LocalMatch> before_clip = matches;

    for (std::size_t i = 0; i < points.size(); ++i) {
        double wsum = 0.0, sx = 0.0, sy = 0.0;
        for (std::size_t j = 0; j < points.size(); ++j) {
            if (i == j) continue;
            const double d2 = distance_sq(points[i], points[j]);
            if (d2 > range_sq) continue;
            const double w = 1.0 / (std::sqrt(d2) + 1.0);
            wsum += w;
            sx += w * before_clip[j].dx;
            sy += w * before_clip[j].dy;
        }
        if (wsum <= 0.0) continue;

        const double mx = sx / wsum;
        const double my = sy / wsum;
        const double ex = before_clip[i].dx - mx;
        const double ey = before_clip[i].dy - my;
        const double dist = std::sqrt(ex * ex + ey * ey);
        if (dist > limit && dist > 1e-9) {
            // 完全に近傍平均へ置き換えるのではなく、許容量までの範囲に引き戻す。
            // 置き換えてしまうと、実際に局所的な歪みがある領域まで平滑化される。
            const double scale = limit / dist;
            matches[i].dx = static_cast<float>(mx + ex * scale);
            matches[i].dy = static_cast<float>(my + ey * scale);
        }
    }
}

}  // namespace stackcore
