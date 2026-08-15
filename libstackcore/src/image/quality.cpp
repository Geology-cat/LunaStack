#include "stackcore/quality.hpp"

#include <Accelerate/Accelerate.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace stackcore {
namespace {

// [1,4,6,4,1]/16。二項係数はσ≈1のガウスによく一致し、整数係数なので
// 環境間で同じ結果になる（決定論性の要件。実装計画書 §4.2）。
constexpr float k0 = 6.0f / 16.0f;
constexpr float k1 = 4.0f / 16.0f;
constexpr float k2 = 1.0f / 16.0f;

int clamp_index(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

int next_pow2(int value) {
    int result = 1;
    while (result < value) result <<= 1;
    return result;
}

vDSP_Length log2_of(int value) {
    vDSP_Length result = 0;
    while ((1 << result) < value) ++result;
    return result;
}

double gradient_energy_preblurred(const float* blurred, int width, int height,
                                  std::size_t stride) {
    double sum = 0.0;
    for (int y = 1; y < height - 1; ++y) {
        const float* prev = blurred + static_cast<std::size_t>(y - 1) * stride;
        const float* cur = blurred + static_cast<std::size_t>(y) * stride;
        const float* next = blurred + static_cast<std::size_t>(y + 1) * stride;
        for (int x = 1; x < width - 1; ++x) {
            const double gx = 0.5 * (static_cast<double>(cur[x + 1]) - cur[x - 1]);
            const double gy = 0.5 * (static_cast<double>(next[x]) - prev[x]);
            sum += gx * gx + gy * gy;
        }
    }
    const double count = static_cast<double>(width - 2) * static_cast<double>(height - 2);
    return count > 0.0 ? sum / count : 0.0;
}

}  // namespace

QualityWorkspace::~QualityWorkspace() {
    if (fft_setup != nullptr) {
        vDSP_destroy_fftsetup(reinterpret_cast<FFTSetup>(fft_setup));
        fft_setup = nullptr;
    }
}

void gaussian_blur_5tap(const float* src, float* dst, int width, int height,
                        std::size_t stride, float* scratch) {
    if (width <= 0 || height <= 0) return;

    // 横方向。端は値を複製する。
    for (int y = 0; y < height; ++y) {
        const float* s = src + static_cast<std::size_t>(y) * stride;
        float* t = scratch + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            const float a = s[clamp_index(x - 2, 0, width - 1)];
            const float b = s[clamp_index(x - 1, 0, width - 1)];
            const float c = s[x];
            const float d = s[clamp_index(x + 1, 0, width - 1)];
            const float e = s[clamp_index(x + 2, 0, width - 1)];
            t[x] = k2 * a + k1 * b + k0 * c + k1 * d + k2 * e;
        }
    }

    // 縦方向。
    for (int y = 0; y < height; ++y) {
        const float* r0 = scratch + static_cast<std::size_t>(clamp_index(y - 2, 0, height - 1)) * stride;
        const float* r1 = scratch + static_cast<std::size_t>(clamp_index(y - 1, 0, height - 1)) * stride;
        const float* r2 = scratch + static_cast<std::size_t>(y) * stride;
        const float* r3 = scratch + static_cast<std::size_t>(clamp_index(y + 1, 0, height - 1)) * stride;
        const float* r4 = scratch + static_cast<std::size_t>(clamp_index(y + 2, 0, height - 1)) * stride;
        float* t = dst + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            t[x] = k2 * r0[x] + k1 * r1[x] + k0 * r2[x] + k1 * r3[x] + k2 * r4[x];
        }
    }
}

void QualityWorkspace::ensure(std::size_t floats) {
    if (blurred.size() < floats) blurred.reset(floats);
    if (scratch.size() < floats) scratch.reset(floats);
}

double gradient_energy(const FrameBuffer& frame, QualityWorkspace& ws) {
    if (frame.empty()) throw std::invalid_argument("gradient_energy: 空のフレームです");

    const int w = frame.width();
    const int h = frame.height();
    const std::size_t stride = frame.stride();
    if (w < 3 || h < 3) return 0.0;  // 中心差分が取れない

    ws.ensure(stride * static_cast<std::size_t>(h));
    const float* luma = frame.luma();
    float* blurred = ws.blurred.data();
    gaussian_blur_5tap(luma, blurred, w, h, stride, ws.scratch.data());

    // 最外周は中心差分が定義できないので除外する。
    // 加算順は y,x の昇順に固定する（決定論性の要件）。
    return gradient_energy_preblurred(blurred, w, h, stride);
}

double frequency_band_power_ratio_preblurred(const float* src, int width, int height,
                                             std::size_t stride, QualityWorkspace& ws) {
    if (src == nullptr || width < 8 || height < 8) return 0.0;

    const int size = next_pow2(std::max(width, height));
    if (ws.fft_size != size) {
        if (ws.fft_setup != nullptr) {
            vDSP_destroy_fftsetup(reinterpret_cast<FFTSetup>(ws.fft_setup));
        }
        ws.fft_setup = vDSP_create_fftsetup(log2_of(size), FFT_RADIX2);
        if (ws.fft_setup == nullptr) {
            ws.fft_size = 0;
            throw std::runtime_error("品質評価: vDSPのFFTセットアップ確保に失敗しました");
        }
        ws.fft_size = size;
        const std::size_t total = static_cast<std::size_t>(size) * size;
        ws.fft_real.reset(total);
        ws.fft_imag.reset(total);
    }

    if (ws.window_width != width || ws.window_height != height) {
        ws.window_x.resize(static_cast<std::size_t>(width));
        ws.window_y.resize(static_cast<std::size_t>(height));
        for (int x = 0; x < width; ++x) {
            ws.window_x[static_cast<std::size_t>(x)] =
                static_cast<float>(0.5 * (1.0 - std::cos(2.0 * M_PI * x / (width - 1))));
        }
        for (int y = 0; y < height; ++y) {
            ws.window_y[static_cast<std::size_t>(y)] =
                static_cast<float>(0.5 * (1.0 - std::cos(2.0 * M_PI * y / (height - 1))));
        }
        ws.window_width = width;
        ws.window_height = height;
    }

    double mean = 0.0;
    for (int y = 0; y < height; ++y) {
        const float* row = src + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < width; ++x) mean += row[x];
    }
    mean /= static_cast<double>(width) * height;

    const std::size_t total = static_cast<std::size_t>(size) * size;
    std::memset(ws.fft_real.data(), 0, total * sizeof(float));
    std::memset(ws.fft_imag.data(), 0, total * sizeof(float));
    for (int y = 0; y < height; ++y) {
        const float* source = src + static_cast<std::size_t>(y) * stride;
        float* destination = ws.fft_real.data() + static_cast<std::size_t>(y) * size;
        const float wy = ws.window_y[static_cast<std::size_t>(y)];
        for (int x = 0; x < width; ++x) {
            destination[x] = static_cast<float>((source[x] - mean) *
                                                ws.window_x[static_cast<std::size_t>(x)] * wy);
        }
    }

    DSPSplitComplex split{ws.fft_real.data(), ws.fft_imag.data()};
    vDSP_fft2d_zip(reinterpret_cast<FFTSetup>(ws.fft_setup), &split, 1, 0, log2_of(size),
                   log2_of(size), FFT_FORWARD);

    double band = 0.0;
    double usable = 0.0;
    const double half = size / 2.0;
    for (int y = 0; y < size; ++y) {
        const double fy = (y >= size / 2 ? y - size : y) / half;
        for (int x = 0; x < size; ++x) {
            const double fx = (x >= size / 2 ? x - size : x) / half;
            const double radius = std::sqrt(fx * fx + fy * fy);
            if (radius < 0.02 || radius > 0.50) continue;
            const std::size_t i = static_cast<std::size_t>(y) * size + x;
            const double real = ws.fft_real.data()[i];
            const double imag = ws.fft_imag.data()[i];
            const double power = real * real + imag * imag;
            usable += power;
            if (radius >= 0.15) band += power;
        }
    }
    return usable > 1e-30 ? band / usable : 0.0;
}

double frequency_band_power_ratio(const FrameBuffer& frame, QualityWorkspace& ws) {
    if (frame.empty()) {
        throw std::invalid_argument("frequency_band_power_ratio: 空のフレームです");
    }
    const int width = frame.width();
    const int height = frame.height();
    const std::size_t stride = frame.stride();
    ws.ensure(stride * static_cast<std::size_t>(height));
    gaussian_blur_5tap(frame.luma(), ws.blurred.data(), width, height, stride, ws.scratch.data());
    return frequency_band_power_ratio_preblurred(ws.blurred.data(), width, height, stride, ws);
}

double quality_score(const FrameBuffer& frame, QualityMetric metric, QualityWorkspace& ws) {
    return metric == QualityMetric::FrequencyBandPowerRatio
               ? frequency_band_power_ratio(frame, ws)
               : gradient_energy(frame, ws);
}

}  // namespace stackcore
