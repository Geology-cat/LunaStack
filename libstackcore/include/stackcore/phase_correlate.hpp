#pragma once

#include <cstddef>
#include <memory>

namespace stackcore {

struct CorrelationPeak {
    // 現在フレームをこれだけ動かすと参照に重なる、という整数変位。
    int dx = 0;
    int dy = 0;
    // 相関面のピーク値（逆FFTを正規化したもの）。
    double peak = 0.0;

    // ピーク対サイドローブ比 (peak - 平均) / 標準偏差。
    // 平均と標準偏差はピーク近傍を除いた相関面から求める。
    //
    // 単純な「ピーク ÷ 相関面の平均」も試したが、白色化を弱めると相関面全体が
    // なだらかになるため、良否の差が実測で 6.6 対 4.5 までしか開かず
    // しきい値を置けなかった。ピーク近傍を除いた分散で正規化すると
    // 「背景のゆらぎに対してピークが何σ突き出ているか」を測ることになり、
    // 相関面の滑らかさに左右されにくくなる。
    double peak_sidelobe_ratio = 0.0;
};

// 位相相関による平行移動推定。
//
// M1では vDSP の複素FFT（`vDSP_fft2d_zip`）を使う。実FFT（`vDSP_fft2d_zrip`）は
// 演算量が半分で済むが、DC項とナイキスト項が1要素に詰め込まれるパック表現になるため、
// 相互パワースペクトルの要素ごとの積で取り違えを起こしやすい。
// グローバルアライメントは1フレームあたり2回のFFTしか使わず支配的でないので、
// ここでは正しさと単純さを優先する。APごとに多数のFFTを回すM2で
// 実FFTへの最適化を検討する（実装計画書 §4.1）。
//
// パディングは**正方**の2の冪にする。`vDSP_fft2d_zip` は行方向・列方向の
// log2長を別々に取るため、非正方だと引数の取り違えが起きても
// 動いてしまう組み合わせがある。正方にしておけば取り違え自体が起こらない。
struct PhaseCorrelateSettings {
    // 白色化の強さ。1.0 で純粋な位相相関（振幅を完全に捨てる）、
    // 0.0 で通常の相互相関（振幅そのまま）。
    //
    // 純粋な位相相関は暗背景の広い惑星動画で破綻する。全周波数の振幅を1に
    // 揃えるということは、信号がほとんど無くノイズしかない高周波を、
    // 惑星本体の低周波と同じ重みまで増幅するということであり、
    // 視野の大半が背景（＝ノイズ）である構図ではノイズが相関面を支配する。
    // 木星SER（448x448、背景62%、8bit）200フレームでの実測。
    // 「変位の最大値」は、独立に測った輝度重心の移動量（dx 2.3 / dy 4.0 px）と
    // 突き合わせる。それを大きく超える値はノイズを拾っている証拠になる。
    //
    //   白色化 低域σ | 変位の中央値 変位の最大 | 20px超のフレーム
    //   1.00   なし  |     29           85     | 116/199   ← 破綻
    //   0.70   なし  |      4           14     |   0/199
    //   0.50   なし  |      3            8     |   0/199
    //   0.30   なし  |      2            6     |   0/199
    //   0.50   0.35  |      2            6     |   0/199
    //   0.30   0.35  |      2            4     |   0/199   ← 重心と一致
    double whitening = 0.30;

    // ガウス低域通過の強さ。ナイキスト周波数を1としたときの標準偏差。
    // 0以下で無効。ノイズが乗る最高周波数帯を落とす。
    double lowpass_sigma = 0.35;
};

class PhaseCorrelator {
public:
    PhaseCorrelator(int width, int height,
                    const PhaseCorrelateSettings& settings = PhaseCorrelateSettings{});
    ~PhaseCorrelator();

    PhaseCorrelator(const PhaseCorrelator&) = delete;
    PhaseCorrelator& operator=(const PhaseCorrelator&) = delete;

    int padded_size() const noexcept;

    // 検出できる変位の上限（±この値）。循環相関なのでこれを超える変位は
    // 折り返して誤った値になる。上流で追跡失敗として扱うこと。
    int max_shift() const noexcept;

    // 参照画像を設定する。以降の correlate() はこれに対する変位を返す。
    void set_reference(const float* ref, int width, int height, std::size_t stride);

    CorrelationPeak correlate(const float* img, int width, int height, std::size_t stride);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace stackcore
