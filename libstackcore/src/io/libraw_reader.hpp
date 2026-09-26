#pragma once

// LibRaw（third_party/LibRaw）を使うカメラRAWの読み込み（エンジン内部用）。
//
// カメラのRAWはすべてここで読む（CR2・CR3・NEF・ARW・RAF・ORF・RW2・PEF・DNG など）。
// LibRaw から使うのは RAW の展開と、機種ごとの切り抜き・黒・白の情報だけで、
// ホワイトバランス・色変換・ガンマは掛けない（リニアのまま 0..1 にする）。

#include <string>

#include "stackcore/frame_buffer.hpp"
#include "stackcore/image_reader.hpp"

namespace stackcore {
namespace detail {

// LibRaw で読む拡張子か（小文字で渡す）。
bool libraw_extension(const std::string& ext);

ImageFileInfo libraw_probe(const std::string& path);
void libraw_read(const std::string& path, FrameBuffer& out, ImageFileInfo& info);

}  // namespace detail
}  // namespace stackcore
