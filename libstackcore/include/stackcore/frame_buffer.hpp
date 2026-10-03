#pragma once

#include <cstddef>

namespace stackcore {

// 32バイト境界に揃えた float バッファ。
// C++17 の aligned operator new は macOS 10.14+ を要求するうえ、10.12・10.13 ターゲットでは
// clang が自動的に無効化する（＝黙って非アラインの new になる）ことを確認済みのため、
// posix_memalign を直接使う。
class AlignedFloats {
public:
    AlignedFloats() noexcept = default;
    explicit AlignedFloats(std::size_t count);
    ~AlignedFloats();

    AlignedFloats(const AlignedFloats&) = delete;
    AlignedFloats& operator=(const AlignedFloats&) = delete;
    AlignedFloats(AlignedFloats&& other) noexcept;
    AlignedFloats& operator=(AlignedFloats&& other) noexcept;

    // 確保しなおしてゼロ初期化する。
    void reset(std::size_t count);
    void release() noexcept;

    float* data() noexcept { return data_; }
    const float* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

private:
    float* data_ = nullptr;
    std::size_t size_ = 0;
};

// 画像1枚分のバッファ。
//
// 規約（実装計画書 §4.7。パイプライン全段がこれを継承するため後から変更しないこと）
//   * float32 の平面（planar）配置。チャンネルごとに連続して並ぶ。
//   * 画素値は 0.0〜1.0 に正規化する。入力のビット深度差はここより上流で吸収し、
//     品質閾値やDenoise閾値が入力深度で意味を変えないようにする。
//   * 各行の先頭は32バイト境界に揃える。1行あたりの float 数は stride() であり
//     width() とは一致しない。要素アクセスは必ず row() / plane() 経由で行うこと。
//   * luma は遅延生成しキャッシュする。モノクロ入力では変換せず実体を共有する。
//
// 画素を直接書き換えた場合は invalidate_luma() を呼ぶこと。
class FrameBuffer {
public:
    FrameBuffer() noexcept = default;
    FrameBuffer(int width, int height, int channels);

    // 確保しなおしてゼロ初期化する。source_bit_depth は既定値に戻る。
    void reset(int width, int height, int channels);
    void clear() noexcept;

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    int channels() const noexcept { return channels_; }
    std::size_t stride() const noexcept { return stride_; }
    bool empty() const noexcept { return data_.empty(); }

    float* plane(int c) noexcept {
        return data_.data() + static_cast<std::size_t>(c) * plane_floats_;
    }
    const float* plane(int c) const noexcept {
        return data_.data() + static_cast<std::size_t>(c) * plane_floats_;
    }
    float* row(int c, int y) noexcept {
        return plane(c) + static_cast<std::size_t>(y) * stride_;
    }
    const float* row(int c, int y) const noexcept {
        return plane(c) + static_cast<std::size_t>(y) * stride_;
    }

    // 元データのビット深度。診断表示と出力時の情報保持に使う。
    // 画素値はすでに 0..1 に正規化済みであり、この値は正規化に影響しない。
    int source_bit_depth() const noexcept { return source_bit_depth_; }
    void set_source_bit_depth(int bits) noexcept { source_bit_depth_ = bits; }

    // 輝度平面（Rec.709）。channels()==1 のときは plane(0) をそのまま返す（コピーしない）。
    // 行の並びは本体と同じ stride() を使う。
    const float* luma() const;
    void invalidate_luma() noexcept { luma_valid_ = false; }

private:
    int width_ = 0;
    int height_ = 0;
    int channels_ = 0;
    std::size_t stride_ = 0;
    std::size_t plane_floats_ = 0;
    AlignedFloats data_;
    int source_bit_depth_ = 16;

    mutable AlignedFloats luma_;
    mutable bool luma_valid_ = false;
};

}  // namespace stackcore
