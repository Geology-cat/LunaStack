#pragma once

#include <cstddef>
#include <vector>

#include "stackcore/frame_buffer.hpp"

namespace stackcore {

enum class QualityMetric {
    GradientEnergy,
    FrequencyBandPowerRatio,
};

// σ≈1 相当の分離可能ガウスぼかし（二項係数 [1,4,6,4,1]/16）。
// 境界は端の値を複製（clamp）する。src と dst は別バッファであること。
// scratch は stride*height 要素以上を持つこと。
void gaussian_blur_5tap(const float* src, float* dst, int width, int height,
                        std::size_t stride, float* scratch);

// 品質評価で使い回す作業バッファ。
// 数千フレームを評価するため、フレームごとに確保しなおさない。
struct QualityWorkspace {
    AlignedFloats blurred;
    AlignedFloats scratch;

    // 周波数帯パワー比を選んだときだけ確保するFFT作業領域。
    // FFTSetupはAccelerateの型を公開ヘッダへ漏らさないためvoid*で保持する。
    void* fft_setup = nullptr;
    int fft_size = 0;
    int window_width = 0;
    int window_height = 0;
    AlignedFloats fft_real;
    AlignedFloats fft_imag;
    std::vector<float> window_x;
    std::vector<float> window_y;

    QualityWorkspace() = default;
    ~QualityWorkspace();
    QualityWorkspace(const QualityWorkspace&) = delete;
    QualityWorkspace& operator=(const QualityWorkspace&) = delete;

    // 必要量に満たないときだけ確保しなおす。
    void ensure(std::size_t floats);
};

// フレーム全体の品質スコア（仕様書 §4.3 の勾配エネルギー）。
//
// 仕様は Q = Σ|∇I|² だが、ここでは**画素数で割った平均**を返す。
// 同一動画内のフレーム同士の順位は割り算で変わらないうえ、
// 切り出しサイズの違う結果同士を比べられるようにするため。
//
// ノイズ過敏を避けるため σ≈1 のぼかし後に計算する（仕様書 §4.3）。
// 勾配は中心差分で求め、差分が定義できない最外周1画素は評価に含めない。
// 入力が0..1正規化済みであること（§4.7）を前提に、返り値も深度非依存になる。
double gradient_energy(const FrameBuffer& frame, QualityWorkspace& ws);

// FFTの中〜高周波帯（0.15〜0.50 Nyquist）のパワーが、
// 有効帯域（0.02〜0.50 Nyquist）に占める割合。軽いぼかし後に評価する。
double frequency_band_power_ratio(const FrameBuffer& frame, QualityWorkspace& ws);

// すでにガウスぼかし済みのAP領域を評価する内部パイプライン向け関数。
double frequency_band_power_ratio_preblurred(const float* src, int width, int height,
                                             std::size_t stride, QualityWorkspace& ws);

double quality_score(const FrameBuffer& frame, QualityMetric metric, QualityWorkspace& ws);

}  // namespace stackcore
