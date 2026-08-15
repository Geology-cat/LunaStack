#pragma once

#include "stackcore/frame_buffer.hpp"
#include "stackcore/ser_decoder.hpp"

namespace stackcore {

// 単純な bilinear デバイヤー。
// M0では「実データを目視で確認できること」が目的であり、高品質版（VNG/AHD相当）は
// v1.x で差し替える。cfa は1ch、out は3ch（R,G,B）になる。
// 未対応パターン（CYYM等）の場合は std::runtime_error を投げる。
void debayer_bilinear(const FrameBuffer& cfa, SerColorId pattern, FrameBuffer& out);

}  // namespace stackcore
