#include "stackcore/jpeg_decoder.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace stackcore {
namespace {

constexpr int kZigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

// C(u)cos((2x+1)uπ/16) をQ14へ丸めた行列。
// 実行時にcosを計算しないので、Intel/ARMで同じ整数IDCTになる。
constexpr std::int32_t kIdct[8][8] = {
    {11585, 16069, 15137, 13623, 11585, 9102, 6270, 3196},
    {11585, 13623, 6270, -3196, -11585, -16069, -15137, -9102},
    {11585, 9102, -6270, -16069, -11585, 3196, 15137, 13623},
    {11585, 3196, -15137, -9102, 11585, 13623, -6270, -16069},
    {11585, -3196, -15137, 9102, 11585, -13623, -6270, 16069},
    {11585, -9102, -6270, 16069, -11585, -3196, 15137, -13623},
    {11585, -13623, 6270, 3196, -11585, 16069, -15137, 9102},
    {11585, -16069, 15137, -13623, 11585, -9102, 6270, -3196},
};

// JPEG仕様の既定Huffman表。AVI MJPEGにはDHTを省略し、この表を暗黙に
// 要求する書き手がある。フレーム内にDHTがあれば、そちらを優先する。
constexpr std::uint8_t kDcLumaCounts[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
constexpr std::uint8_t kDcChromaCounts[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
constexpr std::uint8_t kDcValues[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr std::uint8_t kAcLumaCounts[16] = {0, 2, 1, 3, 3, 2, 4, 3,
                                            5, 5, 4, 4, 0, 0, 1, 0x7D};
constexpr std::uint8_t kAcLumaValues[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
    0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08,
    0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3,
    0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6,
    0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9,
    0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4,
    0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA,
};
constexpr std::uint8_t kAcChromaCounts[16] = {0, 2, 1, 2, 4, 4, 3, 4,
                                              7, 5, 4, 4, 0, 1, 2, 0x77};
constexpr std::uint8_t kAcChromaValues[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,
    0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0, 0x15, 0x62, 0x72, 0xD1,
    0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
    0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44,
    0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74,
    0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A,
    0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4,
    0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7,
    0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
    0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4,
    0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA,
};

int clamp_byte(std::int64_t v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<int>(v);
}

std::int64_t rounded_shift(std::int64_t value, int bits) {
    const std::int64_t half = static_cast<std::int64_t>(1) << (bits - 1);
    if (value >= 0) return (value + half) >> bits;
    return -(((-value) + half) >> bits);
}

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size, std::size_t position)
        : data_(data), size_(size), pos_(position) {}

    int bit() {
        if (bits_ == 0) {
            current_ = entropy_byte();
            bits_ = 8;
        }
        --bits_;
        return (current_ >> bits_) & 1;
    }

    unsigned bits(int count) {
        unsigned out = 0;
        for (int i = 0; i < count; ++i) out = (out << 1) | static_cast<unsigned>(bit());
        return out;
    }

    void restart(int expected) {
        bits_ = 0;
        if (pos_ >= size_ || data_[pos_] != 0xFF) {
            throw std::runtime_error("JPEG: 再スタートマーカーが見つかりません");
        }
        while (pos_ < size_ && data_[pos_] == 0xFF) ++pos_;
        if (pos_ >= size_ || data_[pos_] != static_cast<std::uint8_t>(0xD0 + expected)) {
            throw std::runtime_error("JPEG: 再スタートマーカーの順序が不正です");
        }
        ++pos_;
    }

private:
    int entropy_byte() {
        if (pos_ >= size_) throw std::runtime_error("JPEG: エントロピーデータが途中で終わっています");
        const int value = data_[pos_++];
        if (value != 0xFF) return value;

        while (pos_ < size_ && data_[pos_] == 0xFF) ++pos_;
        if (pos_ >= size_) throw std::runtime_error("JPEG: マーカーが途中で終わっています");
        const int marker = data_[pos_++];
        if (marker == 0x00) return 0xFF;
        throw std::runtime_error("JPEG: 走査データ中に予期しないマーカーがあります");
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_;
    int current_ = 0;
    int bits_ = 0;
};

struct HuffmanTable {
    bool valid = false;
    std::array<int, 17> minimum{};
    std::array<int, 17> maximum{};
    std::array<int, 17> value_offset{};
    std::vector<std::uint8_t> values;

    void build(const std::uint8_t* counts, const std::uint8_t* symbols, int symbol_count) {
        values.assign(symbols, symbols + symbol_count);
        minimum.fill(-1);
        maximum.fill(-1);
        value_offset.fill(0);

        int code = 0;
        int offset = 0;
        for (int length = 1; length <= 16; ++length) {
            const int count = counts[length - 1];
            if (count > 0) {
                if (code + count > (1 << length)) {
                    throw std::runtime_error("JPEG: Huffman表が過剰割り当てです");
                }
                minimum[length] = code;
                maximum[length] = code + count - 1;
                value_offset[length] = offset;
                offset += count;
            }
            code = (code + count) << 1;
        }
        if (offset != symbol_count) throw std::runtime_error("JPEG: Huffman表の長さが不正です");
        valid = true;
    }

    int decode(BitReader& bits) const {
        if (!valid) throw std::runtime_error("JPEG: 必要なHuffman表がありません");
        int code = 0;
        for (int length = 1; length <= 16; ++length) {
            code = (code << 1) | bits.bit();
            if (maximum[length] >= 0 && code >= minimum[length] && code <= maximum[length]) {
                const int index = value_offset[length] + code - minimum[length];
                return values[static_cast<std::size_t>(index)];
            }
        }
        throw std::runtime_error("JPEG: Huffman符号が不正です");
    }
};

struct Component {
    int id = 0;
    int h = 0;
    int v = 0;
    int quant = 0;
    int dc_table = 0;
    int ac_table = 0;
    int dc_predictor = 0;
    int plane_width = 0;
    int plane_height = 0;
    std::vector<std::uint8_t> plane;
};

int receive_extend(BitReader& bits, int length) {
    if (length == 0) return 0;
    if (length < 0 || length > 16) throw std::runtime_error("JPEG: 係数のビット長が不正です");
    const int value = static_cast<int>(bits.bits(length));
    const int threshold = 1 << (length - 1);
    return value < threshold ? value - ((1 << length) - 1) : value;
}

void inverse_dct(const std::array<std::int64_t, 64>& coefficient, std::uint8_t* output) {
    bool dc_only = true;
    for (int i = 1; i < 64; ++i) {
        if (coefficient[static_cast<std::size_t>(i)] != 0) {
            dc_only = false;
            break;
        }
    }
    if (dc_only) {
        const int value = clamp_byte(128 + rounded_shift(coefficient[0], 3));
        for (int i = 0; i < 64; ++i) output[i] = static_cast<std::uint8_t>(value);
        return;
    }

    std::int64_t temporary[8][8] = {};
    for (int v = 0; v < 8; ++v) {
        for (int x = 0; x < 8; ++x) {
            std::int64_t sum = 0;
            for (int u = 0; u < 8; ++u) {
                sum += coefficient[static_cast<std::size_t>(v * 8 + u)] * kIdct[x][u];
            }
            temporary[v][x] = sum;
        }
    }
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            std::int64_t sum = 0;
            for (int v = 0; v < 8; ++v) sum += temporary[v][x] * kIdct[y][v];
            output[y * 8 + x] = static_cast<std::uint8_t>(clamp_byte(128 + rounded_shift(sum, 30)));
        }
    }
}

class Decoder {
public:
    Decoder(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    void decode(FrameBuffer& out) {
        if (size_ < 4 || data_[0] != 0xFF || data_[1] != 0xD8) {
            throw std::runtime_error("JPEG: SOIマーカーがありません");
        }
        pos_ = 2;
        while (pos_ < size_) {
            const int marker = next_marker();
            if (marker == 0xD9) throw std::runtime_error("JPEG: 画像走査データがありません");
            if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
            const std::size_t end = segment_end();
            switch (marker) {
                case 0xDB: parse_quantization(end); break;
                case 0xC0: parse_frame(end); break;
                case 0xC2:
                    throw std::runtime_error("JPEG: progressive JPEGには対応していません");
                case 0xC4: parse_huffman(end); break;
                case 0xDD: parse_restart_interval(end); break;
                case 0xEE: parse_adobe(end); break;
                case 0xDA:
                    parse_scan(end);
                    decode_scan(out);
                    return;
                default: pos_ = end; break;
            }
            if (pos_ != end) pos_ = end;
        }
        throw std::runtime_error("JPEG: SOSマーカーがありません");
    }

private:
    std::uint16_t be16(std::size_t at) const {
        if (at + 2 > size_) throw std::runtime_error("JPEG: データが途中で終わっています");
        return static_cast<std::uint16_t>((static_cast<unsigned>(data_[at]) << 8) | data_[at + 1]);
    }

    int next_marker() {
        while (pos_ < size_ && data_[pos_] != 0xFF) ++pos_;
        if (pos_ >= size_) throw std::runtime_error("JPEG: マーカーが見つかりません");
        while (pos_ < size_ && data_[pos_] == 0xFF) ++pos_;
        if (pos_ >= size_) throw std::runtime_error("JPEG: マーカーが途中で終わっています");
        const int marker = data_[pos_++];
        if (marker == 0x00) return next_marker();
        return marker;
    }

    std::size_t segment_end() {
        const std::uint16_t length = be16(pos_);
        if (length < 2) throw std::runtime_error("JPEG: セグメント長が不正です");
        const std::size_t end = pos_ + length;
        pos_ += 2;
        if (end > size_) throw std::runtime_error("JPEG: セグメントが途中で終わっています");
        return end;
    }

    void parse_quantization(std::size_t end) {
        while (pos_ < end) {
            const int info = data_[pos_++];
            const int precision = info >> 4;
            const int table = info & 15;
            if (table > 3 || precision > 1) throw std::runtime_error("JPEG: DQTの指定が不正です");
            const std::size_t bytes = precision ? 128 : 64;
            if (pos_ + bytes > end) throw std::runtime_error("JPEG: DQTが途中で終わっています");
            for (int i = 0; i < 64; ++i) {
                const int value = precision ? be16(pos_ + static_cast<std::size_t>(i) * 2)
                                            : data_[pos_ + static_cast<std::size_t>(i)];
                if (value == 0) throw std::runtime_error("JPEG: 量子化係数が0です");
                quant_[table][kZigzag[i]] = value;
            }
            quant_valid_[table] = true;
            pos_ += bytes;
        }
    }

    void parse_frame(std::size_t end) {
        if (!components_.empty()) throw std::runtime_error("JPEG: SOF0が複数あります");
        if (pos_ + 6 > end || data_[pos_] != 8) {
            throw std::runtime_error("JPEG: 8bitベースラインJPEGではありません");
        }
        ++pos_;
        height_ = be16(pos_);
        width_ = be16(pos_ + 2);
        pos_ += 4;
        const int count = data_[pos_++];
        const std::int64_t pixels = static_cast<std::int64_t>(width_) * height_;
        if (width_ <= 0 || height_ <= 0 || pixels > 100000000 ||
            (count != 1 && count != 3) ||
            pos_ + static_cast<std::size_t>(count) * 3 != end) {
            throw std::runtime_error("JPEG: SOF0の寸法または成分数が不正です");
        }
        components_.resize(static_cast<std::size_t>(count));
        max_h_ = max_v_ = 0;
        int total_blocks = 0;
        for (Component& component : components_) {
            component.id = data_[pos_++];
            const int sampling = data_[pos_++];
            component.h = sampling >> 4;
            component.v = sampling & 15;
            component.quant = data_[pos_++];
            if (component.h < 1 || component.h > 4 || component.v < 1 || component.v > 4 ||
                component.quant > 3) {
                throw std::runtime_error("JPEG: サンプリング係数が不正です");
            }
            max_h_ = std::max(max_h_, component.h);
            max_v_ = std::max(max_v_, component.v);
            total_blocks += component.h * component.v;
        }
        if (total_blocks > 10) throw std::runtime_error("JPEG: 1 MCUのブロック数が多すぎます");
    }

    void parse_huffman(std::size_t end) {
        while (pos_ < end) {
            const int info = data_[pos_++];
            const int table_class = info >> 4;
            const int table = info & 15;
            if (table_class > 1 || table > 3 || pos_ + 16 > end) {
                throw std::runtime_error("JPEG: DHTの指定が不正です");
            }
            const std::uint8_t* counts = data_ + pos_;
            pos_ += 16;
            int symbol_count = 0;
            for (int i = 0; i < 16; ++i) symbol_count += counts[i];
            if (symbol_count <= 0 || symbol_count > 256 ||
                pos_ + static_cast<std::size_t>(symbol_count) > end) {
                throw std::runtime_error("JPEG: DHTのシンボル数が不正です");
            }
            HuffmanTable& destination = table_class == 0 ? dc_[table] : ac_[table];
            destination.build(counts, data_ + pos_, symbol_count);
            pos_ += static_cast<std::size_t>(symbol_count);
        }
    }

    void parse_restart_interval(std::size_t end) {
        if (pos_ + 2 != end) throw std::runtime_error("JPEG: DRIの長さが不正です");
        restart_interval_ = be16(pos_);
        pos_ += 2;
    }

    void parse_adobe(std::size_t end) {
        if (end - pos_ >= 12 && data_[pos_] == 'A' && data_[pos_ + 1] == 'd' &&
            data_[pos_ + 2] == 'o' && data_[pos_ + 3] == 'b' && data_[pos_ + 4] == 'e') {
            adobe_transform_ = data_[pos_ + 11];
        }
        pos_ = end;
    }

    Component* component_with_id(int id) {
        for (Component& component : components_) {
            if (component.id == id) return &component;
        }
        return nullptr;
    }

    void parse_scan(std::size_t end) {
        if (components_.empty()) throw std::runtime_error("JPEG: SOSより前にSOF0がありません");
        if (pos_ >= end) throw std::runtime_error("JPEG: SOSが途中で終わっています");
        const int count = data_[pos_++];
        if (count != static_cast<int>(components_.size()) ||
            pos_ + static_cast<std::size_t>(count) * 2 + 3 != end) {
            throw std::runtime_error("JPEG: 複数スキャンJPEGには対応していません");
        }
        scan_components_.clear();
        for (int i = 0; i < count; ++i) {
            Component* component = component_with_id(data_[pos_++]);
            const int tables = data_[pos_++];
            if (!component || (tables >> 4) > 3 || (tables & 15) > 3) {
                throw std::runtime_error("JPEG: SOSの成分またはHuffman表が不正です");
            }
            component->dc_table = tables >> 4;
            component->ac_table = tables & 15;
            scan_components_.push_back(component);
        }
        const int spectral_start = data_[pos_++];
        const int spectral_end = data_[pos_++];
        const int approximation = data_[pos_++];
        if (spectral_start != 0 || spectral_end != 63 || approximation != 0) {
            throw std::runtime_error("JPEG: ベースライン逐次走査ではありません");
        }
    }

    void decode_block(Component& component, BitReader& bits, std::uint8_t* pixels) {
        if (!quant_valid_[component.quant]) throw std::runtime_error("JPEG: 必要な量子化表がありません");
        std::array<std::int64_t, 64> coefficient{};
        const int dc_length = dc_[component.dc_table].decode(bits);
        if (dc_length > 11) throw std::runtime_error("JPEG: DC係数カテゴリが不正です");
        component.dc_predictor += receive_extend(bits, dc_length);
        coefficient[0] = static_cast<std::int64_t>(component.dc_predictor) *
                         quant_[component.quant][0];

        int k = 1;
        while (k < 64) {
            const int rs = ac_[component.ac_table].decode(bits);
            const int run = rs >> 4;
            const int length = rs & 15;
            if (length == 0) {
                if (run == 0) break;
                if (run != 15) throw std::runtime_error("JPEG: AC係数の符号が不正です");
                k += 16;
                continue;
            }
            k += run;
            if (k >= 64) throw std::runtime_error("JPEG: AC係数がブロック範囲を超えています");
            const int natural = kZigzag[k];
            coefficient[static_cast<std::size_t>(natural)] =
                static_cast<std::int64_t>(receive_extend(bits, length)) *
                quant_[component.quant][natural];
            ++k;
        }
        inverse_dct(coefficient, pixels);
    }

    static int floor_q8(int value) {
        return value >= 0 ? value / 256 : -((-value + 255) / 256);
    }

    int sample(const Component& component, int x, int y) const {
        const int sx_q8 = ((2 * x + 1) * component.h * 256) / (2 * max_h_) - 128;
        const int sy_q8 = ((2 * y + 1) * component.v * 256) / (2 * max_v_) - 128;
        int x0 = floor_q8(sx_q8);
        int y0 = floor_q8(sy_q8);
        const int fx = sx_q8 - x0 * 256;
        const int fy = sy_q8 - y0 * 256;
        int x1 = x0 + 1;
        int y1 = y0 + 1;
        x0 = std::max(0, std::min(component.plane_width - 1, x0));
        x1 = std::max(0, std::min(component.plane_width - 1, x1));
        y0 = std::max(0, std::min(component.plane_height - 1, y0));
        y1 = std::max(0, std::min(component.plane_height - 1, y1));

        const int p00 = component.plane[static_cast<std::size_t>(y0) * component.plane_width + x0];
        const int p10 = component.plane[static_cast<std::size_t>(y0) * component.plane_width + x1];
        const int p01 = component.plane[static_cast<std::size_t>(y1) * component.plane_width + x0];
        const int p11 = component.plane[static_cast<std::size_t>(y1) * component.plane_width + x1];
        const int top = p00 * (256 - fx) + p10 * fx;
        const int bottom = p01 * (256 - fx) + p11 * fx;
        return (top * (256 - fy) + bottom * fy + 32768) >> 16;
    }

    void install_default_huffman_tables() {
        if (!dc_[0].valid) dc_[0].build(kDcLumaCounts, kDcValues, 12);
        if (!ac_[0].valid) ac_[0].build(kAcLumaCounts, kAcLumaValues, 162);
        if (!dc_[1].valid) dc_[1].build(kDcChromaCounts, kDcValues, 12);
        if (!ac_[1].valid) ac_[1].build(kAcChromaCounts, kAcChromaValues, 162);
    }

    void decode_scan(FrameBuffer& out) {
        install_default_huffman_tables();
        for (Component* component : scan_components_) {
            if (!dc_[component->dc_table].valid || !ac_[component->ac_table].valid) {
                throw std::runtime_error("JPEG: 必要なHuffman表がありません");
            }
            component->dc_predictor = 0;
        }

        const int mcu_width = max_h_ * 8;
        const int mcu_height = max_v_ * 8;
        const int columns = (width_ + mcu_width - 1) / mcu_width;
        const int rows = (height_ + mcu_height - 1) / mcu_height;
        for (Component& component : components_) {
            component.plane_width = columns * component.h * 8;
            component.plane_height = rows * component.v * 8;
            component.plane.assign(static_cast<std::size_t>(component.plane_width) *
                                       component.plane_height,
                                   0);
        }

        BitReader bits(data_, size_, pos_);
        int mcu = 0;
        int restart_marker = 0;
        std::uint8_t block[64];
        for (int my = 0; my < rows; ++my) {
            for (int mx = 0; mx < columns; ++mx, ++mcu) {
                if (restart_interval_ > 0 && mcu > 0 && mcu % restart_interval_ == 0) {
                    bits.restart(restart_marker);
                    restart_marker = (restart_marker + 1) & 7;
                    for (Component& component : components_) component.dc_predictor = 0;
                }
                for (Component* component : scan_components_) {
                    for (int by = 0; by < component->v; ++by) {
                        for (int bx = 0; bx < component->h; ++bx) {
                            decode_block(*component, bits, block);
                            const int px = (mx * component->h + bx) * 8;
                            const int py = (my * component->v + by) * 8;
                            for (int y = 0; y < 8; ++y) {
                                std::uint8_t* destination = component->plane.data() +
                                    static_cast<std::size_t>(py + y) * component->plane_width + px;
                                std::copy(block + y * 8, block + y * 8 + 8, destination);
                            }
                        }
                    }
                }
            }
        }

        const int channels = components_.size() == 1 ? 1 : 3;
        out.reset(width_, height_, channels);
        out.set_source_bit_depth(8);
        const float inverse = 1.0f / 255.0f;
        if (channels == 1) {
            for (int y = 0; y < height_; ++y) {
                float* destination = out.row(0, y);
                for (int x = 0; x < width_; ++x) destination[x] = sample(components_[0], x, y) * inverse;
            }
        } else {
            // Adobe APP14のtransform=0は3成分ならRGB。IDが'R','G','B'でない
            // ファイルもあるため、成分IDだけで判定しない。
            const bool direct_rgb = adobe_transform_ == 0 ||
                                    (components_[0].id == 'R' && components_[1].id == 'G' &&
                                     components_[2].id == 'B');
            for (int y = 0; y < height_; ++y) {
                float* red = out.row(0, y);
                float* green = out.row(1, y);
                float* blue = out.row(2, y);
                for (int x = 0; x < width_; ++x) {
                    const int first = sample(components_[0], x, y);
                    const int second = sample(components_[1], x, y);
                    const int third = sample(components_[2], x, y);
                    int r, g, b;
                    if (direct_rgb) {
                        r = first;
                        g = second;
                        b = third;
                    } else {
                        const int cb = second - 128;
                        const int cr = third - 128;
                        r = clamp_byte(first + rounded_shift(91881LL * cr, 16));
                        g = clamp_byte(first - rounded_shift(22554LL * cb + 46802LL * cr, 16));
                        b = clamp_byte(first + rounded_shift(116130LL * cb, 16));
                    }
                    red[x] = r * inverse;
                    green[x] = g * inverse;
                    blue[x] = b * inverse;
                }
            }
        }
        out.invalidate_luma();
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
    int width_ = 0;
    int height_ = 0;
    int max_h_ = 0;
    int max_v_ = 0;
    int restart_interval_ = 0;
    int adobe_transform_ = -1;
    std::array<std::array<int, 64>, 4> quant_{};
    std::array<bool, 4> quant_valid_{};
    std::array<HuffmanTable, 4> dc_;
    std::array<HuffmanTable, 4> ac_;
    std::vector<Component> components_;
    std::vector<Component*> scan_components_;
};

}  // namespace

void decode_baseline_jpeg(const std::uint8_t* data, std::size_t size, FrameBuffer& out) {
    if (!data) throw std::invalid_argument("JPEG: 入力データがnullです");
    Decoder decoder(data, size);
    decoder.decode(out);
}

}  // namespace stackcore
