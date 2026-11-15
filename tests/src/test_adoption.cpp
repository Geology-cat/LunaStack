#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "microtest.hpp"
#include "stackcore/adoption_diagnosis.hpp"
#include "stackcore/frame_buffer.hpp"
#include "stackcore/map_pipeline.hpp"
#include "stackcore/video_source.hpp"

using stackcore::AdoptionDiagnosis;
using stackcore::FrameBuffer;
using stackcore::MapStackSettings;
using stackcore::SerColorId;
using stackcore::VideoSource;

namespace {

struct Lcg {
    std::uint32_t s;
    explicit Lcg(std::uint32_t seed) : s(seed) {}
    double next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<double>((s >> 8) & 0xFFFF) / 65535.0;
    }
};

// ガウス斑点を散らした場面を、斑点の広がり sigma で描く（sigma が大きいほど甘い像）。
// 斑点の光量は保つ（広げたぶん暗くなる）。
std::vector<float> render_scene(int extent, double sigma, std::uint32_t seed) {
    Lcg rng(seed);
    const int count = extent * extent / 50;
    std::vector<float> image(static_cast<std::size_t>(extent) * extent, 0.15f);
    const double amplitude = 0.25 * (1.3 * 1.3) / (sigma * sigma);
    const int reach = static_cast<int>(std::ceil(4.0 * sigma));
    for (int i = 0; i < count; ++i) {
        const double bx = rng.next() * extent, by = rng.next() * extent;
        const double a = amplitude * (0.3 + 0.7 * rng.next());
        for (int y = std::max(0, static_cast<int>(by) - reach); y <= std::min(extent - 1, static_cast<int>(by) + reach); ++y) {
            for (int x = std::max(0, static_cast<int>(bx) - reach); x <= std::min(extent - 1, static_cast<int>(bx) + reach); ++x) {
                const double r2 = (x - bx) * (x - bx) + (y - by) * (y - by);
                image[static_cast<std::size_t>(y) * extent + x] += static_cast<float>(a * std::exp(-r2 / (2.0 * sigma * sigma)));
            }
        }
    }
    return image;
}

// フレームごとに像の甘さとノイズを決められる合成動画。
// blurred_every が 2 なら、4枚のうち後ろの2枚が甘い（交互に2組へ分けても、どちらにも半分ずつ入る）。
class QualitySource : public VideoSource {
public:
    QualitySource(int size, int frames, bool mix_blurred, double noise, std::uint32_t seed) : size_(size) {
        const int margin = 8;
        const int extent = size + 2 * margin;
        const std::vector<float> sharp = render_scene(extent, 1.3, seed);
        const std::vector<float> soft = render_scene(extent, 3.2, seed);
        Lcg rng(seed + 11);
        for (int n = 0; n < frames; ++n) {
            const bool blurred = mix_blurred && (n % 4) >= 2;
            const std::vector<float>& base = blurred ? soft : sharp;
            const int ox = margin + static_cast<int>(rng.next() * 5.0) - 2;
            const int oy = margin + static_cast<int>(rng.next() * 5.0) - 2;
            FrameBuffer f(size, size, 1);
            for (int y = 0; y < size; ++y) {
                float* row = f.row(0, y);
                for (int x = 0; x < size; ++x) {
                    double v = base[static_cast<std::size_t>(y + oy) * extent + (x + ox)];
                    v += noise * (rng.next() - 0.5);
                    row[x] = static_cast<float>(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
                }
            }
            f.invalidate_luma();
            frames_.push_back(std::move(f));
        }
    }

    int width() const override { return size_; }
    int height() const override { return size_; }
    int frame_count() const override { return static_cast<int>(frames_.size()); }
    SerColorId color_id() const override { return SerColorId::Mono; }
    int bit_depth() const override { return 16; }
    bool has_timestamps() const override { return false; }
    std::int64_t timestamp_ticks(int) const override { return 0; }
    stackcore::FrameStats frame_stats(int) const override { return stackcore::FrameStats{}; }
    const char* format_name() const override { return "synthetic"; }
    std::string describe() const override { return "合成"; }
    void read_frame(int index, FrameBuffer& out) const override {
        const FrameBuffer& src = frames_[static_cast<std::size_t>(index)];
        if (out.width() != size_ || out.height() != size_ || out.channels() != 1) out.reset(size_, size_, 1);
        for (int y = 0; y < size_; ++y) {
            const float* s = src.row(0, y);
            float* d = out.row(0, y);
            for (int x = 0; x < size_; ++x) d[x] = s[x];
        }
        out.invalidate_luma();
    }

private:
    int size_;
    std::vector<FrameBuffer> frames_;
};

MapStackSettings test_settings() {
    MapStackSettings settings;
    settings.ap.ap_size = 32;
    settings.local.search_radius = 8;
    settings.reference_passes = 1;
    return settings;
}

double score_at(const AdoptionDiagnosis& d, double percent) {
    for (const stackcore::AdoptionPoint& p : d.points) {
        if (p.percent == percent) return p.score;
    }
    return -1.0;
}

void print_points(const AdoptionDiagnosis& d) {
    std::printf("           ");
    for (const stackcore::AdoptionPoint& p : d.points) std::printf("%.0f%%=%.3f ", p.percent, p.score);
    std::printf("→ 目安 %.0f%%\n", d.suggested_percent);
}

}  // namespace

MT_TEST(adoption_甘いフレームが半分混ざりノイズが小さければ少なめを勧める) {
    // 鋭いフレームと甘いフレームが半々。ノイズは小さいので、甘いフレームまで使うと細部が鈍るだけになる。
    const QualitySource source(160, 48, true, 0.01, 31);
    const MapStackSettings settings = test_settings();
    stackcore::MapStackReport report;
    const stackcore::AnalysisData analysis = stackcore::analyze_map_stack(source, settings, nullptr, report);
    const AdoptionDiagnosis d = stackcore::diagnose_adoption(source, settings, analysis, nullptr);
    MT_CHECK(!d.insufficient);
    print_points(d);
    MT_CHECK(d.suggested_percent <= 50.0);
    MT_CHECK(score_at(d, 50.0) > score_at(d, 100.0));
}

MT_TEST(adoption_どのフレームも同じ質でノイズが大きければ多めを勧める) {
    const QualitySource source(160, 48, false, 0.08, 32);
    const MapStackSettings settings = test_settings();
    stackcore::MapStackReport report;
    const stackcore::AnalysisData analysis = stackcore::analyze_map_stack(source, settings, nullptr, report);
    const AdoptionDiagnosis d = stackcore::diagnose_adoption(source, settings, analysis, nullptr);
    MT_CHECK(!d.insufficient);
    print_points(d);
    MT_CHECK(d.suggested_percent >= 75.0);
    MT_CHECK(score_at(d, 100.0) > score_at(d, 25.0));
    MT_CHECK(score_at(d, 25.0) > score_at(d, 10.0));
    // APあたりの枚数は、全フレームでスタックするときの値。
    MT_CHECK_EQ(d.points.back().frames_per_ap, 48);
}

MT_TEST(adoption_同じ入力なら結果は同じで途中で中断もできる) {
    const QualitySource source(160, 32, true, 0.05, 33);
    const MapStackSettings settings = test_settings();
    stackcore::MapStackReport report;
    const stackcore::AnalysisData analysis = stackcore::analyze_map_stack(source, settings, nullptr, report);
    const AdoptionDiagnosis a = stackcore::diagnose_adoption(source, settings, analysis, nullptr);
    const AdoptionDiagnosis b = stackcore::diagnose_adoption(source, settings, analysis, nullptr);
    MT_CHECK_EQ(a.points.size(), b.points.size());
    for (std::size_t i = 0; i < a.points.size() && i < b.points.size(); ++i) {
        MT_CHECK_EQ(a.points[i].score, b.points[i].score);
    }
    MT_CHECK_EQ(a.suggested_percent, b.suggested_percent);

    int calls = 0;
    bool cancelled = false;
    try {
        stackcore::diagnose_adoption(source, settings, analysis, [&calls](const char*, int, int) { return ++calls < 3; });
    } catch (const stackcore::Cancelled&) {
        cancelled = true;
    }
    MT_CHECK(cancelled);
}

MT_TEST(adoption_フレームが少なすぎれば判断しない) {
    const QualitySource source(160, 12, false, 0.05, 34);
    const MapStackSettings settings = test_settings();
    stackcore::MapStackReport report;
    const stackcore::AnalysisData analysis = stackcore::analyze_map_stack(source, settings, nullptr, report);
    const AdoptionDiagnosis d = stackcore::diagnose_adoption(source, settings, analysis, nullptr);
    MT_CHECK(d.insufficient);
}
