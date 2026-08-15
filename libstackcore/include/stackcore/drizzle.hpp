#pragma once

#include <cstdint>
#include <vector>

#include "stackcore/frame_buffer.hpp"

namespace stackcore {

struct DrizzleStats {
    std::size_t uncovered_pixels = 0;  // どのフレームからも寄与を受けなかった出力画素
    double min_weight = 0.0;
    double max_weight = 0.0;
    double max_value = 0.0;
    std::size_t clipped = 0;
    int frames = 0;
};

// Drizzle（仕様書 §4.9）。
//
// 出力グリッドを scale 倍に拡大し、入力画素を「面積を持った四角」として
// サブピクセル変位込みで投影する。窓合成と同じ S / W の2バッファ方式。
//
// **Drizzleが効くのはアンダーサンプリングのときだけ。**
// 焦点距離が十分長くて元々オーバーサンプリングなら、
// 拡大しても情報は増えず、処理時間とメモリが増えるだけになる。
// UIではこれを注意書きとして出すこと（仕様書 §4.9）。
//
// pixfrac は入力画素を縮めてから落とす割合（既定0.9）。
// 1.0だと隣の出力画素にまたがって滲み、小さくするほど鋭くなるが、
// 小さくしすぎると寄与を受けない出力画素が出る（穴が開く）。
class Drizzle {
public:
    // in_width/in_height は入力フレームの寸法。
    // scale は 1.0 / 1.5 / 2.0 / 3.0 を想定（任意の正数を受け付ける）。
    Drizzle(int in_width, int in_height, int channels, double scale, double pixfrac);

    int out_width() const noexcept { return out_width_; }
    int out_height() const noexcept { return out_height_; }

    // frame を (dx, dy) だけ動かした位置として投影する。
    // 変位は**入力画素の単位**で与える（出力画素ではない）。
    // gain は輝度正規化の係数。
    //
    // 呼び出し順はフレーム番号の昇順に固定すること（決定論性の要件）。
    void add(const FrameBuffer& frame, double dx, double dy, double gain);

    void finish(FrameBuffer& out, DrizzleStats& stats) const;

private:
    int in_width_ = 0, in_height_ = 0, channels_ = 0;
    int out_width_ = 0, out_height_ = 0;
    double scale_ = 1.0;
    double pixfrac_ = 0.9;
    int frames_ = 0;

    std::vector<double> sum_;     // channels_ 面ぶん
    std::vector<double> weight_;  // 1面（変位は全チャンネル共通）
};

}  // namespace stackcore
