#pragma once

#include <memory>
#include <vector>

#include "stackcore/frame_buffer.hpp"
#include "stackcore/wavelet.hpp"

namespace stackcore {

// ---- 仕上げ（後処理）の各工程 ---------------------------------------------
//
// スタック結果に対する非破壊の後処理（UI設計書 §1.5）。掛ける順序は固定:
//   1. RGBチャンネルの位置合わせ（大気分散の補正）
//   2. ウェーブレット（細部強調・ノイズ低減）＋デリンギング
//   3. 色（ホワイトバランス・彩度）
//   4. ヒストグラムストレッチ（黒点・白点・ガンマ）
//   5. 形（クロップ → 回転 → 反転）
// チャンネル合わせは色ずれした輪郭を強調しないよう、ウェーブレットより前に置く。
// 形の変更は画素値を変えないので最後に置く。

// RGBのチャンネルずれ（Gを基準にしたR・Bの変位、画素単位）。
//
// 惑星は地平高度が低いと大気の分散で青が上に、赤が下にずれる。
// 各チャンネルを別々に動かして重ねると、縁の色にじみが消える。
struct ChannelOffsets {
    double red_dx = 0.0;
    double red_dy = 0.0;
    double blue_dx = 0.0;
    double blue_dy = 0.0;

    bool any() const {
        return red_dx != 0.0 || red_dy != 0.0 || blue_dx != 0.0 || blue_dy != 0.0;
    }
    bool operator==(const ChannelOffsets& o) const {
        return red_dx == o.red_dx && red_dy == o.red_dy && blue_dx == o.blue_dx &&
               blue_dy == o.blue_dy;
    }
    bool operator!=(const ChannelOffsets& o) const { return !(*this == o); }
};

// R・BをGに重ねるための変位を推定する（±max_shift画素、0.01画素単位）。
// 対象の明るい領域（最大512×512）でZNCCを総当たりし、放物線でサブピクセル推定する。
// 3ch以外なら0を返す。
ChannelOffsets estimate_channel_offsets(const FrameBuffer& rgb, int max_shift = 8);

// R・Bを指定の変位だけ動かす（Lanczos3）。
// 「dx だけずれているのを戻す」向きに動かす: 出力(x) = 入力(x + dx)。
void shift_channels(const FrameBuffer& src, const ChannelOffsets& offsets, FrameBuffer& out);

// 色の調整。
struct ColorAdjust {
    double gain[3] = {1.0, 1.0, 1.0};  // R, G, B の倍率
    double saturation = 1.0;           // 1.0 で変化なし、0 でモノクロ
    bool identity() const {
        return gain[0] == 1.0 && gain[1] == 1.0 && gain[2] == 1.0 && saturation == 1.0;
    }
};

// 灰色仮説のホワイトバランス。背景（暗い画素）を除いた対象の平均色が
// 灰色になる倍率を返す（Gを1とする）。3ch以外なら {1,1,1}。
void estimate_white_balance(const FrameBuffer& rgb, double gains[3]);

// src と out に同じ画像を渡すと、その場で書き換える。
void apply_color(const FrameBuffer& src, const ColorAdjust& color, FrameBuffer& out);

// 形の変更。クロップは**回転前の**画像座標で指定する（自動クロップの結果を
// そのまま使えるようにするため）。
struct Geometry {
    bool crop = false;
    int crop_x = 0;
    int crop_y = 0;
    int crop_width = 0;
    int crop_height = 0;
    int rotate_quarter_turns = 0;  // 時計回りに90°×n（0〜3）
    bool flip_horizontal = false;  // 回転のあとで左右反転
    bool flip_vertical = false;    // 回転のあとで上下反転

    bool identity() const {
        return !crop && (rotate_quarter_turns % 4) == 0 && !flip_horizontal && !flip_vertical;
    }
};

// 対象（惑星・月面の明るい部分）を囲む矩形。背景を除く自動クロップに使う。
// margin は四方に足す余白（画素）。対象が見つからなければ画像全体を返す。
void detect_object_bounds(const FrameBuffer& image, int margin, int& x, int& y, int& width,
                          int& height);

void apply_geometry(const FrameBuffer& src, const Geometry& geometry, FrameBuffer& out);

// ウェーブレット強調で明るい縁の外側にできる暗い輪（リンギング）を抑える。
//
// 強調後の値が、元画像の近傍（半径 radius）の最小〜最大から外へはみ出した分を
// strength（0〜1）の割合だけ削る。1なら近傍の範囲に完全に収める。
// 平坦な場所の細部強調は近傍の範囲内に収まるので、効くのは主に輪郭の外側である。
void dering(const FrameBuffer& original, double strength, int radius, FrameBuffer& sharpened);

// 仕上げの設定一式。
struct FinishingSettings {
    ChannelOffsets channels;
    std::vector<WaveletLayerParams> wavelet;  // 空ならウェーブレットを掛けない
    double dering = 0.0;                      // 0〜1
    ColorAdjust color;
    bool stretch = false;
    double black = 0.0;
    double white = 1.0;
    double gamma = 1.0;
    Geometry geometry;

    // すべてが「変化なし」か（仕上げを通しても入力と同じになるか）。
    bool identity() const;
};

// 仕上げの処理系。
//
// **プレビューと書き出しは必ずこれを通す。** 別々に組み立てると、
// 画面で見た結果と保存したファイルが食い違う。
//
// 重い工程（チャンネル合わせとウェーブレット分解）の結果を保持し、
// 設定が変わった工程から後ろだけをやり直す。スライダー操作では通常
// ウェーブレットの再構成から後ろしか走らない。
class FinishingPipeline {
public:
    // 入力を差し替える。キャッシュはすべて捨てる。
    void set_input(std::shared_ptr<const FrameBuffer> stacked, int wavelet_layers = 6);
    bool ready() const { return static_cast<bool>(input_); }
    const FrameBuffer& input() const { return *input_; }

    // 仕上げ済みの画像を作る。
    void render(const FinishingSettings& settings, FrameBuffer& out);

    // ウェーブレット直前の画像（チャンネル合わせ済み）。自動推定の入力に使う。
    const FrameBuffer& aligned() ;

private:
    void ensure_aligned(const ChannelOffsets& offsets);

    std::shared_ptr<const FrameBuffer> input_;
    int layers_ = 6;
    bool aligned_valid_ = false;
    ChannelOffsets aligned_offsets_;
    FrameBuffer aligned_;
    bool wavelet_valid_ = false;
    WaveletSharpener wavelet_;
    // デリンギングの許容範囲（半径ごと。0 は未計算）。
    int dering_radius_ = 0;
    std::vector<float> dering_lo_, dering_hi_;
    // 形を変えるときの途中の画像（使い回す）。
    FrameBuffer work_;
};

}  // namespace stackcore
