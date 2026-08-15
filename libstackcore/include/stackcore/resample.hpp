#pragma once

#include <cstddef>

#include "stackcore/frame_buffer.hpp"

namespace stackcore {

// Lanczos3 による小数変位での切り出し（仕様書 §4.3）。
//
// vImage を使わず自前カーネルにしているのは、境界処理と決定論性を自分で
// 制御するためである（実装計画書 §4.3）。OSやバージョンによって内部実装が
// 変わると「同一入力・同一設定なら同一品質」の保証が崩れる。
//
// 出力の (ox, oy) 画素は、入力の (src_x0 + ox + frac_x, src_y0 + oy + frac_y) を
// 標本化した値になる。frac は -1..1 程度の小数を想定している。
//
// 境界の扱い: 参照する6x6タップが入力の外に出る場合は端の値を複製する（clamp）。
// 0で埋めると縁が暗くなり、オーバーラップ窓合成で継ぎ目として現れるため。
//
// out_stride は出力バッファの1行あたりの要素数。
void resample_lanczos3(const float* src, int src_width, int src_height,
                       std::size_t src_stride, double src_x0, double src_y0, float* dst,
                       int dst_width, int dst_height, std::size_t dst_stride);

// 整数位置の切り出し（小数部が0のときの最適化経路）。
// resample_lanczos3 と同じ境界処理を行う。
void copy_patch_clamped(const float* src, int src_width, int src_height,
                        std::size_t src_stride, int src_x0, int src_y0, float* dst,
                        int dst_width, int dst_height, std::size_t dst_stride);

// Lanczos3 カーネル。テストから直接検証できるよう公開している。
double lanczos3_kernel(double x);

}  // namespace stackcore
