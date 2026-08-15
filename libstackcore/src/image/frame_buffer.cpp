#include "stackcore/frame_buffer.hpp"

#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>

namespace stackcore {
namespace {

constexpr std::size_t kAlignBytes = 32;
constexpr std::size_t kAlignFloats = kAlignBytes / sizeof(float);

std::size_t align_up(std::size_t value, std::size_t multiple) {
    return (value + multiple - 1) / multiple * multiple;
}

}  // namespace

AlignedFloats::AlignedFloats(std::size_t count) { reset(count); }

AlignedFloats::~AlignedFloats() { release(); }

AlignedFloats::AlignedFloats(AlignedFloats&& other) noexcept
    : data_(other.data_), size_(other.size_) {
    other.data_ = nullptr;
    other.size_ = 0;
}

AlignedFloats& AlignedFloats::operator=(AlignedFloats&& other) noexcept {
    if (this != &other) {
        release();
        data_ = other.data_;
        size_ = other.size_;
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

void AlignedFloats::release() noexcept {
    std::free(data_);
    data_ = nullptr;
    size_ = 0;
}

void AlignedFloats::reset(std::size_t count) {
    release();
    if (count == 0) return;
    void* p = nullptr;
    if (posix_memalign(&p, kAlignBytes, count * sizeof(float)) != 0 || p == nullptr) {
        throw std::bad_alloc();
    }
    data_ = static_cast<float*>(p);
    size_ = count;
    std::memset(data_, 0, count * sizeof(float));
}

FrameBuffer::FrameBuffer(int width, int height, int channels) {
    reset(width, height, channels);
}

void FrameBuffer::reset(int width, int height, int channels) {
    if (width <= 0 || height <= 0 || channels <= 0) {
        throw std::invalid_argument("FrameBuffer: 幅・高さ・チャンネル数は正の値である必要があります");
    }
    width_ = width;
    height_ = height;
    channels_ = channels;
    stride_ = align_up(static_cast<std::size_t>(width), kAlignFloats);
    plane_floats_ = stride_ * static_cast<std::size_t>(height);
    data_.reset(plane_floats_ * static_cast<std::size_t>(channels));
    luma_.release();
    luma_valid_ = false;
    source_bit_depth_ = 16;
}

void FrameBuffer::clear() noexcept {
    if (!data_.empty()) {
        std::memset(data_.data(), 0, data_.size() * sizeof(float));
    }
    luma_valid_ = false;
}

const float* FrameBuffer::luma() const {
    if (data_.empty()) return nullptr;

    // モノクロ入力では変換自体が不要なので実体を共有する。
    if (channels_ == 1) return data_.data();

    if (luma_valid_ && luma_.size() == plane_floats_) return luma_.data();

    luma_.reset(plane_floats_);
    float* dst = luma_.data();

    if (channels_ >= 3) {
        const float* r = plane(0);
        const float* g = plane(1);
        const float* b = plane(2);
        for (std::size_t i = 0; i < plane_floats_; ++i) {
            dst[i] = 0.2126f * r[i] + 0.7152f * g[i] + 0.0722f * b[i];
        }
    } else {
        const float* c0 = plane(0);
        const float* c1 = plane(1);
        for (std::size_t i = 0; i < plane_floats_; ++i) {
            dst[i] = 0.5f * (c0[i] + c1[i]);
        }
    }

    luma_valid_ = true;
    return luma_.data();
}

}  // namespace stackcore
