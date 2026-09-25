#include "stackcore/calibration.hpp"

#include <stdexcept>
#include <vector>

#include "stackcore/global_stage.hpp"
#include "stackcore/video_source.hpp"

namespace stackcore {

FrameBuffer build_master_frame(const VideoSource& source,
                               const CalibrationProgressFn& progress) {
    const int total = source.frame_count();
    if (total <= 0) throw std::runtime_error("キャリブレーション: フレームがありません");

    FrameBuffer frame;
    source.read_frame(0, frame);
    const int w = frame.width(), h = frame.height(), c = frame.channels();
    std::vector<double> sum(static_cast<std::size_t>(w) * h * c, 0.0);

    for (int i = 0; i < total; ++i) {
        if (i > 0) source.read_frame(i, frame);
        if (frame.width() != w || frame.height() != h || frame.channels() != c) {
            throw std::runtime_error("キャリブレーション: 途中でフレームの寸法が変わりました");
        }
        std::size_t o = 0;
        for (int ch = 0; ch < c; ++ch) {
            for (int y = 0; y < h; ++y) {
                const float* row = frame.row(ch, y);
                for (int x = 0; x < w; ++x) sum[o++] += row[x];
            }
        }
        if (progress && !progress(i + 1, total)) throw Cancelled();
    }

    FrameBuffer master(w, h, c);
    master.set_source_bit_depth(frame.source_bit_depth());
    std::size_t o = 0;
    for (int ch = 0; ch < c; ++ch) {
        for (int y = 0; y < h; ++y) {
            float* row = master.row(ch, y);
            for (int x = 0; x < w; ++x) row[x] = static_cast<float>(sum[o++] / total);
        }
    }
    master.invalidate_luma();
    return master;
}

FrameBuffer normalize_flat(const FrameBuffer& master_flat, SerColorId pattern) {
    if (master_flat.empty()) return FrameBuffer();
    const int w = master_flat.width(), h = master_flat.height(), c = master_flat.channels();
    const bool bayer = c == 1 && is_bayer(pattern);
    // 位相ごと（Bayer）またはチャンネルごとの平均。
    const int groups = bayer ? 4 : c;
    std::vector<double> sum(static_cast<std::size_t>(groups), 0.0);
    std::vector<double> count(static_cast<std::size_t>(groups), 0.0);
    const auto group_of = [&](int ch, int x, int y) {
        return bayer ? ((y & 1) * 2 + (x & 1)) : ch;
    };
    for (int ch = 0; ch < c; ++ch) {
        for (int y = 0; y < h; ++y) {
            const float* row = master_flat.row(ch, y);
            for (int x = 0; x < w; ++x) {
                const int g = group_of(ch, x, y);
                sum[static_cast<std::size_t>(g)] += row[x];
                count[static_cast<std::size_t>(g)] += 1.0;
            }
        }
    }
    std::vector<double> mean(static_cast<std::size_t>(groups), 1.0);
    for (int g = 0; g < groups; ++g) {
        if (count[static_cast<std::size_t>(g)] > 0.0 && sum[static_cast<std::size_t>(g)] > 0.0) {
            mean[static_cast<std::size_t>(g)] =
                sum[static_cast<std::size_t>(g)] / count[static_cast<std::size_t>(g)];
        }
    }
    FrameBuffer flat(w, h, c);
    for (int ch = 0; ch < c; ++ch) {
        for (int y = 0; y < h; ++y) {
            const float* src = master_flat.row(ch, y);
            float* dst = flat.row(ch, y);
            for (int x = 0; x < w; ++x) {
                const double m = mean[static_cast<std::size_t>(group_of(ch, x, y))];
                const double v = src[x] / m;
                dst[x] = v < 0.01 ? 1.0f : static_cast<float>(v);
            }
        }
    }
    flat.invalidate_luma();
    return flat;
}

void apply_calibration(FrameBuffer& frame, const CalibrationFrames& calibration) {
    if (calibration.empty()) return;
    const auto check = [&](const FrameBuffer& ref, const char* name) {
        if (ref.width() != frame.width() || ref.height() != frame.height() ||
            ref.channels() != frame.channels()) {
            throw std::runtime_error(std::string("キャリブレーション: ") + name +
                                     "の寸法またはチャンネル数が入力と合いません");
        }
    };
    if (!calibration.dark.empty()) check(calibration.dark, "ダーク");
    if (!calibration.flat.empty()) check(calibration.flat, "フラット");

    const bool has_dark = !calibration.dark.empty();
    const bool has_flat = !calibration.flat.empty();
    for (int ch = 0; ch < frame.channels(); ++ch) {
        for (int y = 0; y < frame.height(); ++y) {
            float* row = frame.row(ch, y);
            const float* dark = has_dark ? calibration.dark.row(ch, y) : nullptr;
            const float* flat = has_flat ? calibration.flat.row(ch, y) : nullptr;
            for (int x = 0; x < frame.width(); ++x) {
                float v = row[x];
                if (dark) v -= dark[x];
                if (flat) v /= flat[x];
                row[x] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            }
        }
    }
    frame.invalidate_luma();
}

}  // namespace stackcore
