#pragma once

#include <string>

#include "stackcore/frame_buffer.hpp"
#include "stackcore/metadata.hpp"

namespace stackcore {

enum class TiffFormat {
    UInt16,   // 0..1 を 0..65535 にスケールして書き出す（既定）
    Float32,  // 0..1 の値をそのまま書き出す（後処理前提の中間出力）
};

// 非圧縮のベースラインTIFFを書き出す（リトルエンディアン・単一ストリップ）。
// 1ch（グレースケール）と3ch（RGB）に対応する。
// 失敗時は std::runtime_error を投げる。
void write_tiff(const std::string& path, const FrameBuffer& image, TiffFormat format);

// メタデータ付き。空なら上と同じバイト列になる。
// ImageDescription・Software・DateTime（撮影時刻。現在時刻ではない）を書く。
void write_tiff(const std::string& path, const FrameBuffer& image, TiffFormat format,
                const ImageMetadata& metadata);

}  // namespace stackcore
