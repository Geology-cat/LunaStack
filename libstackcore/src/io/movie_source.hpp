#pragma once

// MOV / MP4 / M4V の読み込み（エンジン内部用。AVFoundation を使う）。
//
// 仕様書 §3.1: MOV・MP4 は OS 付属のデコーダ（AVFoundation）経由で読む。デコード結果は
// OS の版によって変わりうるので、SER・AVI のような「全OSで同一の画素」は保証しない
// （describe() にもそう書く）。

#include <memory>
#include <string>

namespace stackcore {

class VideoSource;

namespace detail {

// 拡張子（大文字小文字は区別しない）で MOV・MP4・M4V かを判定する。
bool is_movie_path(const std::string& path);

// 開く。失敗時は std::runtime_error を投げる。
std::unique_ptr<VideoSource> open_movie(const std::string& path);

}  // namespace detail
}  // namespace stackcore
