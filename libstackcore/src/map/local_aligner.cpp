#include "stackcore/local_aligner.hpp"

#include <algorithm>
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

double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    const std::size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + middle, values.end());
    const double upper = values[middle];
    if ((values.size() & 1U) != 0U) return upper;
    const double lower = *std::max_element(values.begin(), values.begin() + middle);
    return 0.5 * (lower + upper);
}

}  // namespace

LocalFieldRepairStats repair_displacement_field(const std::vector<AlignmentPoint>& points,
                                                std::vector<LocalMatch>& matches,
                                                int ap_grid_step,
                                                const LocalAlignSettings& settings) {
    if (points.size() != matches.size()) {
        throw std::invalid_argument("外れ値処理: AP数と結果数が一致しません");
    }
    LocalFieldRepairStats stats;
    if (points.empty()) return stats;

    // --- 1. フレーム全体で変位場の一貫性を調べる -------------------------
    //
    // 木星の帯のようなほぼ一方向の模様では、ZNCCが高いまま偽ピークへ滑り、
    // APごとに互いに矛盾する変位を返すことがある。誤った近傍どうしで補間すると
    // 円盤の輪郭まで局所的に引き延ばされるため、補間より前に場全体を検査する。
    std::vector<double> valid_dx;
    std::vector<double> valid_dy;
    valid_dx.reserve(matches.size());
    valid_dy.reserve(matches.size());
    for (const LocalMatch& match : matches) {
        if (!match.valid) continue;
        valid_dx.push_back(match.dx);
        valid_dy.push_back(match.dy);
    }

    const double consensus_x = median(valid_dx);
    const double consensus_y = median(valid_dy);
    std::vector<double> deviations;
    deviations.reserve(valid_dx.size());
    for (std::size_t i = 0; i < valid_dx.size(); ++i) {
        deviations.push_back(
            std::hypot(valid_dx[i] - consensus_x, valid_dy[i] - consensus_y));
    }
    stats.median_deviation = median(deviations);

    // 48px AP（24px間隔）で1.5pxを基準とする。大きなAPでは局所変形の
    // 許容量も比例させるが、細かな格子で過敏にならないよう下限を置く。
    const double spread_limit = std::max(1.5, ap_grid_step / 16.0);
    const bool too_few_valid = valid_dx.size() * 2 < points.size();
    const bool inconsistent =
        valid_dx.empty() || too_few_valid || stats.median_deviation > spread_limit;

    if (inconsistent) {
        // 有効点が少数でも中央値なら単発の偽ピークに引かれにくい。
        // 1点も無い場合の中央値は0で、グローバル位置合わせだけを使う。
        for (LocalMatch& match : matches) {
            match.dx = static_cast<float>(consensus_x);
            match.dy = static_cast<float>(consensus_y);
        }
        stats.used_global_consensus = true;
        return stats;
    }

    // 場全体は一貫していても、孤立した偽ピークはあり得る。中央値から
    // 十分に離れた有効点を無効扱いにし、後段の近傍補間へ回す。
    const double outlier_limit = std::max(spread_limit, 3.0 * stats.median_deviation);
    for (LocalMatch& match : matches) {
        if (!match.valid) continue;
        if (std::hypot(match.dx - consensus_x, match.dy - consensus_y) > outlier_limit) {
            match.valid = false;
            ++stats.consensus_outliers;
        }
    }

    const double range_sq =
        (kNeighborRange * ap_grid_step) * (kNeighborRange * ap_grid_step);

    // --- 2. 無効なAPを近傍の有効APから補間する ----------------------------
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
            // 近傍に有効APが1つもない場合も、場全体の頑健な代表値を使う。
            matches[i].dx = static_cast<float>(consensus_x);
            matches[i].dy = static_cast<float>(consensus_y);
        }
        // valid は false のまま残す。補間で埋めた値であることを
        // 上流（スタック時の重み付けや診断）が知れるようにするため。
    }

    // --- 3. 隣接APとの差が大きすぎる変位をクリップする --------------------
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
    return stats;
}

}  // namespace stackcore
