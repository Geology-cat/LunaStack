#pragma once

#include <string>

#include "stackcore/frame_buffer.hpp"
#include "stackcore/metadata.hpp"

namespace stackcore {

// 16bit PNGを書き出す。1ch（グレースケール）と3ch（RGB）に対応する。
//
// 圧縮ストリームは無圧縮DEFLATEブロックを自前で生成する。ファイルは一般的な
// PNGビューアで読め、OSやzlibの版によって出力バイト列が変わらない。
// 大画像でも全画素を複製せず、最大64KBのブロック単位で書き出す。
// 失敗時は std::runtime_error を投げる。
void write_png16(const std::string& path, const FrameBuffer& image);

// メタデータ付き（tEXtチャンク: Software / Description / Creation Time）。
// 空なら上と同じバイト列になる。
void write_png16(const std::string& path, const FrameBuffer& image,
                 const ImageMetadata& metadata);

}  // namespace stackcore
