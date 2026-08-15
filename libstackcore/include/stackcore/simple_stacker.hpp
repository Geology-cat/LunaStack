#pragma once

#include <cstdint>
#include <vector>

#include "stackcore/frame_buffer.hpp"

namespace stackcore {

struct StackStats {
    int frames = 0;              // 加算したフレーム数
    std::uint32_t min_coverage = 0;  // 最も寄与の少なかった画素の加算回数
    std::uint32_t max_coverage = 0;
    double max_value = 0.0;      // 平均後の最大値（1.0を超えたら輝度正規化が過剰）
    std::size_t clipped = 0;     // 1.0を超えて切り詰めた画素数
};

// 上位N%フレームの単純平均スタック（M1）。
//
// 切り出しは整数変位のみ。サブピクセル補間（Lanczos3）は M2 以降の担当であり
// （実装計画書 §4.3）、M1の出力は原理的に最大0.5px分ぼける。
// これは実装の欠陥ではなくM1の設計上の割り切りである。
//
// アキュムレータは float64 を使う。M2の受け入れ条件が
// 「float32 / float64 / 解析解の比較にもとづき型を決定し根拠を記録する」ことを
// 要求しているため、その測定が済むまでは精度側に倒しておく。
// 448x448x3 で 4.8MB であり、M1の規模ではメモリ上の問題にならない。
class SimpleStacker {
public:
    SimpleStacker(int width, int height, int channels);

    // frame を (dx, dy) だけ動かして加算する。gain は輝度正規化の係数。
    // **呼び出し順はフレーム番号の昇順に固定すること**。float64でも加算は
    // 非結合であり、順序が変わると結果がビット単位で変わって再現性を失う
    // （実装計画書 §4.2 の決定論性要件）。
    void add(const FrameBuffer& frame, int dx, int dy, double gain);

    // 画素ごとの加算回数で割って平均を取り出す。
    // 変位のせいで一度も寄与を受けなかった画素は0になる。
    void finish(FrameBuffer& out, StackStats& stats) const;

    int count() const noexcept { return frames_; }

private:
    int width_ = 0;
    int height_ = 0;
    int channels_ = 0;
    int frames_ = 0;
    std::vector<double> accum_;          // channels_ 面ぶん、width_*height_ 連続
    std::vector<std::uint32_t> coverage_;  // 全チャンネル共通（変位が同じため）
};

}  // namespace stackcore
