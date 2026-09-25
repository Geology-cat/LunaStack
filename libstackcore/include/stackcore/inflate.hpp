#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace stackcore {

// DEFLATE（RFC 1951）の自前デコーダ。
//
// PNG と Deflate圧縮TIFF の読み込みに使う。OS付属のデコーダ（ImageIO）に頼らないのは、
// 動画デコーダと同じく「同一入力なら全OSバージョンで同一の画素」を守るため
// （仕様書 §3.1・§8-7）。可逆圧縮なので正しく実装すれば出力は一意に決まる。
//
// 失敗時は std::runtime_error を投げる。expected_size が 0 でなければ、
// 展開後の大きさがそれを超えた時点で打ち切る（壊れたファイルでメモリを食い潰さない）。
std::vector<std::uint8_t> inflate_raw(const std::uint8_t* data, std::size_t size,
                                      std::size_t expected_size = 0);

// zlib形式（RFC 1950: 2バイトのヘッダ + DEFLATE + Adler-32）を展開する。
// Adler-32 が一致しなければ例外を投げる。
std::vector<std::uint8_t> inflate_zlib(const std::uint8_t* data, std::size_t size,
                                       std::size_t expected_size = 0);

}  // namespace stackcore
