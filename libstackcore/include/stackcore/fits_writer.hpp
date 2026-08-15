#pragma once

#include <string>

#include "stackcore/frame_buffer.hpp"

namespace stackcore {

// FITS Primary HDUへ、[0,1]に正規化したIEEE 754 32bit float画像を書き出す。
//
// 1chはNAXIS=2、RGBはNAXIS=3 / NAXIS3=3の単一データキューブにする。
// RGBの面順はR, G, B。PixInsightが生成する多チャンネルFITSと同じ構造で、
// 3つの独立HDUには分けない。
//
// FITSの既定どおり画素はbig endian、ヘッダとデータは2880バイト境界へ詰める。
// PixInsightの浮動小数点FITS既定範囲に合わせ、非有限値は0、範囲外は[0,1]へ
// クランプする。これにより読み込み時の範囲確認ダイアログを避ける。
void write_fits_float32(const std::string& path, const FrameBuffer& image);

}  // namespace stackcore
