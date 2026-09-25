#pragma once

#include "stackcore/frame_buffer.hpp"
#include "stackcore/ser_decoder.hpp"

namespace stackcore {

// 単純な bilinear デバイヤー。
// M0では「実データを目視で確認できること」が目的であり、高品質版（VNG/AHD相当）は
// v1.x で差し替える。cfa は1ch、out は3ch（R,G,B）になる。
// 未対応パターン（CYYM等）の場合は std::runtime_error を投げる。
void debayer_bilinear(const FrameBuffer& cfa, SerColorId pattern, FrameBuffer& out);

// デバイヤーの方式。
//   Bilinear       : 同色の近傍平均。従来の既定（出力はv0.2系と完全一致）
//   MalvarHeCutler : 他色の勾配で補正する5x5の線形補間（Malvar, He, Cutler 2004）。
//                    bilinearより偽色と輪郭のぼけが少なく、計算量はほぼ同じ
enum class DebayerMethod { Bilinear = 0, MalvarHeCutler = 1 };

const char* to_string(DebayerMethod method);

void debayer_mhc(const FrameBuffer& cfa, SerColorId pattern, FrameBuffer& out);

// 方式を指定してデバイヤーする。
void debayer(const FrameBuffer& cfa, SerColorId pattern, DebayerMethod method, FrameBuffer& out);

}  // namespace stackcore
