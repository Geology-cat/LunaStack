#pragma once

#include <vector>

#include "stackcore/frame_buffer.hpp"

namespace stackcore {

// レイヤーごとの後処理パラメータ（仕様書 §4.10）。
struct WaveletLayerParams {
    // Sharpen係数 g_j（0〜3）。1.0でそのまま、大きくするとその周波数帯を強調する。
    double sharpen = 1.0;
    // Denoise（soft-threshold、0〜1）。
    // しきい値はレイヤー自身のノイズ推定に対する比で効かせるので、
    // 画像の明るさやビット深度が変わっても意味が変わらない。
    double denoise = 0.0;
};

// à trous（undecimated）ウェーブレットによるシャープニング（仕様書 §4.10）。
//
//   分解: c_0 = I, c_j = h_j * c_{j-1}, w_j = c_{j-1} - c_j
//   再構成: I' = c_J + Σ g_j·w_j
//
// ダウンサンプリングしないので位置不変であり、天体のシャープニングに向く。
// レイヤー j のカーネルは B3スプライン [1,4,6,4,1]/16 のタップを
// 2^j 間隔に広げたもの（「穴あき」= à trous）。
//
// **分解と再構成を分けている。** スライダーを動かすたびに分解からやり直すと
// 応答目標（推奨環境 <100ms）を満たせない。分解は1回、再構成だけを繰り返す。
class WaveletSharpener {
public:
    // layers は分解するレイヤー数（仕様書の既定は6）。
    WaveletSharpener();

    // 分解する。重い側の処理。
    void analyze(const FrameBuffer& src, int layers);

    int layers() const noexcept { return layers_; }
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    int channels() const noexcept { return channels_; }
    bool ready() const noexcept { return layers_ > 0; }

    // レイヤー j に残るノイズの推定σ。
    //
    // レイヤーごとに中央絶対偏差を取るのではなく、ノイズが支配的な
    // 最細レイヤーから σ を1つ推定し、à trous のノイズ伝播係数で
    // 各レイヤーへ配分している。粗いレイヤーのMADはノイズではなく
    // 実際の構造の大きさを測ってしまうため。
    double layer_noise(int layer) const;

    // 再構成する。軽い側の処理。params の要素数は layers() と同じであること。
    // すべて sharpen=1.0 / denoise=0.0 なら、出力は入力と一致する（可逆性）。
    void synthesize(const std::vector<WaveletLayerParams>& params, FrameBuffer& out) const;

private:
    int width_ = 0, height_ = 0, channels_ = 0, layers_ = 0;
    std::size_t plane_ = 0;  // 1チャンネルぶんの要素数（width_*height_）

    // w_[j] は j 番目の詳細レイヤー（channels_ 面ぶん連続）。
    std::vector<std::vector<float>> detail_;
    // 最も粗い残差 c_J。
    std::vector<float> residual_;
    std::vector<double> noise_;
};

// ヒストグラムストレッチ（仕様書 §4.10 のv1項目）。
//
// black / white は 0..1 の入力レンジ、gamma は 0 より大きい値。
// out = ((in - black) / (white - black)) ^ (1/gamma)
// 範囲外は 0..1 に切り詰める。
void stretch_histogram(const FrameBuffer& src, double black, double white, double gamma,
                       FrameBuffer& out);

}  // namespace stackcore
