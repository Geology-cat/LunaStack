#include "stackcore/frame_selector.hpp"

#include <algorithm>
#include <cmath>

namespace stackcore {

double similarity_outlier_threshold(const std::vector<double>& similarities, double k,
                                    double floor_margin, std::size_t min_samples) {
    if (similarities.size() < min_samples) return -1.0;

    std::vector<double> sorted = similarities;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2];

    std::vector<double> deviations;
    deviations.reserve(sorted.size());
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        deviations.push_back(std::fabs(sorted[i] - median));
    }
    std::sort(deviations.begin(), deviations.end());
    const double mad = deviations[deviations.size() / 2];

    // 0.6745 で割ると、正規分布のときの標準偏差に一致する尺度になる。
    const double sigma = mad / 0.6745;
    const double threshold = median - k * sigma;
    const double floor_threshold = median - floor_margin;
    return threshold > floor_threshold ? floor_threshold : threshold;
}

}  // namespace stackcore
