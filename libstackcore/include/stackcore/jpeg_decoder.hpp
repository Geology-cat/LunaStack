#pragma once

#include <cstddef>
#include <cstdint>

#include "stackcore/frame_buffer.hpp"

namespace stackcore {

// 8bitベースライン逐次JPEG（SOF0）を自前で展開する。
// ImageIO / AVFoundation等のOSデコーダは使わないため、対応OSによって
// SER/AVIの結果が変わらない。失敗時はstd::runtime_errorを投げる。
void decode_baseline_jpeg(const std::uint8_t* data, std::size_t size, FrameBuffer& out);

}  // namespace stackcore

