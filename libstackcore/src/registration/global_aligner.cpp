#include "stackcore/global_aligner.hpp"

#include <cmath>
#include <cstring>
#include <stdexcept>

#include "stackcore/phase_correlate.hpp"

namespace stackcore {
namespace {

// 重心を取る際の閾値。最小値と最大値の間をこの割合で切る。
// 低すぎると背景ノイズが重心を引っ張り、高すぎると対象の縁が落ちる。
constexpr double kCentroidThresholdRatio = 0.25;

// 自動モード判定で「暗い」とみなす輝度（0..1正規化後）。
constexpr float kDarkLevel = 0.10f;
// 暗い画素がこの割合を超えたら惑星モードとみなす。
constexpr double kDarkFractionForPlanet = 0.5;

}  // namespace

const char* to_string(AlignMode mode) {
    switch (mode) {
        case AlignMode::Auto: return "auto";
        case AlignMode::Planet: return "planet";
        case AlignMode::Lunar: return "lunar";
    }
    return "unknown";
}

const char* to_string(RejectReason reason) {
    switch (reason) {
        case RejectReason::None: return "採用";
        case RejectReason::LowCorrelation: return "参照との類似度不足（追跡失敗）";
        case RejectReason::ShiftTooLarge: return "変位が許容量を超過";
        case RejectReason::StructuralOutlier: return "構造的な外れ値（テアリング等）";
    }
    return "不明";
}

bool luma_centroid(const FrameBuffer& frame, double& cx, double& cy) {
    if (frame.empty()) return false;
    const int w = frame.width(), h = frame.height();
    const std::size_t stride = frame.stride();
    const float* luma = frame.luma();

    float lo = luma[0], hi = luma[0];
    for (int y = 0; y < h; ++y) {
        const float* row = luma + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            if (row[x] < lo) lo = row[x];
            if (row[x] > hi) hi = row[x];
        }
    }
    if (!(hi > lo)) return false;  // 一様な画像には重心が定義できない

    const float threshold =
        lo + static_cast<float>(kCentroidThresholdRatio) * (hi - lo);

    // 加算順は y,x の昇順に固定する（決定論性の要件。実装計画書 §4.2）。
    double sum = 0.0, sx = 0.0, sy = 0.0;
    for (int y = 0; y < h; ++y) {
        const float* row = luma + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            if (row[x] <= threshold) continue;
            // 閾値を引いてから重みにする。背景側の底上げが重心を中央へ
            // 引き寄せるのを避けるため。
            const double wgt = static_cast<double>(row[x] - threshold);
            sum += wgt;
            sx += wgt * x;
            sy += wgt * y;
        }
    }
    if (sum <= 0.0) return false;

    cx = sx / sum;
    cy = sy / sum;
    return true;
}

double mean_luma(const FrameBuffer& frame) {
    if (frame.empty()) return 0.0;
    const int w = frame.width(), h = frame.height();
    const std::size_t stride = frame.stride();
    const float* luma = frame.luma();

    double sum = 0.0;
    for (int y = 0; y < h; ++y) {
        const float* row = luma + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < w; ++x) sum += row[x];
    }
    return sum / (static_cast<double>(w) * h);
}

AlignMode detect_align_mode(const FrameBuffer& reference) {
    if (reference.empty()) return AlignMode::Lunar;
    const int w = reference.width(), h = reference.height();
    const std::size_t stride = reference.stride();
    const float* luma = reference.luma();

    std::size_t dark = 0;
    for (int y = 0; y < h; ++y) {
        const float* row = luma + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            if (row[x] < kDarkLevel) ++dark;
        }
    }
    const double fraction = static_cast<double>(dark) / (static_cast<double>(w) * h);
    return fraction > kDarkFractionForPlanet ? AlignMode::Planet : AlignMode::Lunar;
}

struct GlobalAligner::Impl {
    GlobalAlignSettings settings;
    AlignMode mode = AlignMode::Lunar;
    int width = 0, height = 0;
    int max_shift = 0;
    double ref_cx = 0.0, ref_cy = 0.0;
    bool ref_has_centroid = false;
    PhaseCorrelator correlator;
    AlignedFloats shifted;   // 粗補正後の画像を作る作業バッファ
    AlignedFloats ref_luma;  // ZNCC計算のために参照の輝度を保持する
    std::size_t stride = 0;

    Impl(const FrameBuffer& reference, const GlobalAlignSettings& s)
        : settings(s),
          width(reference.width()),
          height(reference.height()),
          correlator(reference.width(), reference.height()),
          stride(reference.stride()) {
        correlator.set_reference(reference.luma(), width, height, stride);
        ref_has_centroid = luma_centroid(reference, ref_cx, ref_cy);

        const std::size_t plane = stride * static_cast<std::size_t>(height);
        ref_luma.reset(plane);
        std::memcpy(ref_luma.data(), reference.luma(), plane * sizeof(float));

        mode = settings.mode == AlignMode::Auto ? detect_align_mode(reference)
                                                : settings.mode;
        // 重心が取れない（一様な）参照では惑星モードにできない。
        if (mode == AlignMode::Planet && !ref_has_centroid) mode = AlignMode::Lunar;

        const int shorter = width < height ? width : height;
        max_shift = settings.max_shift > 0 ? settings.max_shift : shorter / 4;
        if (max_shift > correlator.max_shift()) max_shift = correlator.max_shift();

        shifted.reset(stride * static_cast<std::size_t>(height));
    }

    // 変位 (dx, dy) で重ねたときの正規化相互相関。
    // 参照の (x, y) と、フレームの (x - dx, y - dy) を突き合わせる。
    // 重なりのない領域は評価に含めない。
    double zncc(const float* frame_luma, int dx, int dy) const {
        const int y0 = dy > 0 ? dy : 0;
        const int y1 = dy < 0 ? height + dy : height;
        const int x0 = dx > 0 ? dx : 0;
        const int x1 = dx < 0 ? width + dx : width;
        if (y1 - y0 < 2 || x1 - x0 < 2) return 0.0;

        double sa = 0.0, sb = 0.0;
        std::size_t count = 0;
        for (int y = y0; y < y1; ++y) {
            const float* a = ref_luma.data() + static_cast<std::size_t>(y) * stride;
            const float* b = frame_luma + static_cast<std::size_t>(y - dy) * stride;
            for (int x = x0; x < x1; ++x) {
                sa += a[x];
                sb += b[x - dx];
                ++count;
            }
        }
        const double ma = sa / static_cast<double>(count);
        const double mb = sb / static_cast<double>(count);

        double num = 0.0, va = 0.0, vb = 0.0;
        for (int y = y0; y < y1; ++y) {
            const float* a = ref_luma.data() + static_cast<std::size_t>(y) * stride;
            const float* b = frame_luma + static_cast<std::size_t>(y - dy) * stride;
            for (int x = x0; x < x1; ++x) {
                const double da = a[x] - ma;
                const double db = b[x - dx] - mb;
                num += da * db;
                va += da * da;
                vb += db * db;
            }
        }
        const double denom = std::sqrt(va * vb);
        // 分散が0（一様なフレーム）のときは似ているとも似ていないとも言えない。
        // 追跡できていないのは確かなので0を返す。
        return denom > 1e-20 ? num / denom : 0.0;
    }

    // dst(x,y) = src(x - dx, y - dy)。範囲外は0で埋める。
    void shift_into(const float* src, int dx, int dy) {
        float* dst = shifted.data();
        std::memset(dst, 0, stride * static_cast<std::size_t>(height) * sizeof(float));
        for (int y = 0; y < height; ++y) {
            const int sy = y - dy;
            if (sy < 0 || sy >= height) continue;
            const float* s = src + static_cast<std::size_t>(sy) * stride;
            float* d = dst + static_cast<std::size_t>(y) * stride;
            for (int x = 0; x < width; ++x) {
                const int sx = x - dx;
                if (sx < 0 || sx >= width) continue;
                d[x] = s[sx];
            }
        }
    }
};

GlobalAligner::GlobalAligner(const FrameBuffer& reference,
                             const GlobalAlignSettings& settings) {
    if (reference.empty()) {
        throw std::invalid_argument("GlobalAligner: 参照フレームが空です");
    }
    impl_.reset(new Impl(reference, settings));
}

GlobalAligner::~GlobalAligner() = default;

AlignMode GlobalAligner::mode() const noexcept { return impl_->mode; }

int GlobalAligner::max_shift() const noexcept { return impl_->max_shift; }

GlobalAlignResult GlobalAligner::align(const FrameBuffer& frame) {
    if (frame.width() != impl_->width || frame.height() != impl_->height ||
        frame.stride() != impl_->stride) {
        throw std::invalid_argument("GlobalAligner: 参照と寸法が異なるフレームです");
    }

    GlobalAlignResult out;
    const float* luma = frame.luma();

    int coarse_dx = 0, coarse_dy = 0;
    const float* target = luma;

    if (impl_->mode == AlignMode::Planet) {
        double cx = 0.0, cy = 0.0;
        if (luma_centroid(frame, cx, cy)) {
            coarse_dx = static_cast<int>(std::lround(impl_->ref_cx - cx));
            coarse_dy = static_cast<int>(std::lround(impl_->ref_cy - cy));
            if (coarse_dx != 0 || coarse_dy != 0) {
                impl_->shift_into(luma, coarse_dx, coarse_dy);
                target = impl_->shifted.data();
            }
        }
        // 重心が取れない場合は粗補正なしで位相相関だけに任せる。
    }

    const CorrelationPeak peak =
        impl_->correlator.correlate(target, impl_->width, impl_->height, impl_->stride);

    out.dx = coarse_dx + peak.dx;
    out.dy = coarse_dy + peak.dy;
    out.peak_sidelobe_ratio = peak.peak_sidelobe_ratio;
    out.similarity = impl_->zncc(luma, out.dx, out.dy);

    if (out.similarity < impl_->settings.min_similarity) {
        out.accepted = false;
        out.reason = RejectReason::LowCorrelation;
    } else if (std::abs(out.dx) > impl_->max_shift ||
               std::abs(out.dy) > impl_->max_shift) {
        out.accepted = false;
        out.reason = RejectReason::ShiftTooLarge;
    } else {
        out.accepted = true;
        out.reason = RejectReason::None;
    }
    return out;
}

}  // namespace stackcore
