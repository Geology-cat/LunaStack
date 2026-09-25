#include "stackcore/inflate.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace stackcore {
namespace {

[[noreturn]] void fail(const char* what) {
    throw std::runtime_error(std::string("DEFLATE: ") + what);
}

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    // LSBから順にビットを取り出す（DEFLATEの規約）。
    unsigned bits(int count) {
        if (count == 0) return 0;
        fill(count);
        if (have_ < count) fail("データが途中で切れています");
        const unsigned value = static_cast<unsigned>(buffer_ & ((1ull << count) - 1u));
        buffer_ >>= count;
        have_ -= count;
        return value;
    }

    // count ビットを先読みする（データの末尾では足りない分を0で埋める）。
    unsigned peek(int count) {
        fill(count);
        return static_cast<unsigned>(buffer_ & ((1ull << count) - 1u));
    }

    void consume(int count) {
        if (have_ < count) fail("データが途中で切れています");
        buffer_ >>= count;
        have_ -= count;
    }

    void align_to_byte() {
        // バッファに残ったビットのうち、バイト境界までの端数だけを捨てる。
        const int drop = have_ % 8;
        buffer_ >>= drop;
        have_ -= drop;
    }

    std::uint8_t byte() {
        if (have_ >= 8) {
            const std::uint8_t b = static_cast<std::uint8_t>(buffer_ & 0xFFu);
            buffer_ >>= 8;
            have_ -= 8;
            return b;
        }
        if (pos_ >= size_) fail("データが途中で切れています");
        return data_[pos_++];
    }

    // バイト境界に揃っているとき、n バイトをまとめて写す（無圧縮ブロック用）。
    void copy_bytes(std::vector<std::uint8_t>& out, std::size_t n, std::size_t limit) {
        while (n > 0 && have_ >= 8) {
            if (limit != 0 && out.size() >= limit) fail("展開後の大きさが想定を超えました");
            out.push_back(byte());
            --n;
        }
        if (n > size_ - pos_) fail("データが途中で切れています");
        if (limit != 0 && out.size() + n > limit) fail("展開後の大きさが想定を超えました");
        out.insert(out.end(), data_ + pos_, data_ + pos_ + n);
        pos_ += n;
    }

private:
    void fill(int count) {
        while (have_ < count && pos_ < size_) {
            buffer_ |= static_cast<std::uint64_t>(data_[pos_++]) << have_;
            have_ += 8;
        }
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
    std::uint64_t buffer_ = 0;
    int have_ = 0;
};

// 正準ハフマン符号の復号表（符号長ごとの個数と、符号順に並べたシンボル）。
// 9ビット以下の符号は先読み表で1回で引き、長い符号だけ1ビットずつ辿る。
constexpr int kFastBits = 9;

struct Huffman {
    unsigned short count[16] = {0};
    unsigned short symbol[288] = {0};
    // 下位9ビット（ストリーム順）→ シンボル | 符号長<<9。0は表に無い（長い符号）。
    unsigned short fast[1 << kFastBits] = {0};
};

void build_huffman(Huffman& h, const unsigned char* lengths, int n) {
    for (int i = 0; i < 16; ++i) h.count[i] = 0;
    for (int i = 0; i < n; ++i) h.count[lengths[i]]++;
    h.count[0] = 0;
    // 過剰な符号（符号空間を超える）は壊れたデータ。
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) fail("ハフマン表が不正です");
    }
    unsigned short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = offs[len] + h.count[len];
    for (int i = 0; i < n; ++i) {
        if (lengths[i] != 0) h.symbol[offs[lengths[i]]++] = static_cast<unsigned short>(i);
    }

    // 先読み表。符号は上位ビットから並ぶが、ストリームは下位ビットから読むので反転して置く。
    for (int i = 0; i < (1 << kFastBits); ++i) h.fast[i] = 0;
    int code = 0, index = 0;
    for (int len = 1; len <= kFastBits; ++len) {
        for (int k = 0; k < h.count[len]; ++k) {
            int reversed = 0;
            for (int b = 0; b < len; ++b) reversed |= ((code >> b) & 1) << (len - 1 - b);
            const unsigned short entry =
                static_cast<unsigned short>(h.symbol[index] | (len << kFastBits));
            for (int fill = reversed; fill < (1 << kFastBits); fill += 1 << len) h.fast[fill] = entry;
            ++code;
            ++index;
        }
        code <<= 1;
    }
}

int decode_symbol(BitReader& in, const Huffman& h) {
    const unsigned short entry = h.fast[in.peek(kFastBits)];
    if (entry != 0) {
        in.consume(entry >> kFastBits);
        return entry & ((1 << kFastBits) - 1);
    }
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= static_cast<int>(in.bits(1));
        const int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    fail("ハフマン符号が不正です");
}

const unsigned short kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                                        15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                                        67, 83, 99, 115, 131, 163, 195, 227, 258};
const unsigned short kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                         2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const unsigned short kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,
                                      17,   25,   33,   49,   65,   97,    129,   193,
                                      257,  385,  513,  769,  1025, 1537,  2049,  3073,
                                      4097, 6145, 8193, 12289, 16385, 24577};
const unsigned short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void push(std::vector<std::uint8_t>& out, std::uint8_t b, std::size_t limit) {
    if (limit != 0 && out.size() >= limit) fail("展開後の大きさが想定を超えました");
    out.push_back(b);
}

void inflate_block(BitReader& in, std::vector<std::uint8_t>& out, const Huffman& lit,
                   const Huffman& dist, std::size_t limit) {
    for (;;) {
        int sym = decode_symbol(in, lit);
        if (sym < 256) {
            push(out, static_cast<std::uint8_t>(sym), limit);
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) fail("長さ符号が不正です");
            const unsigned length = kLengthBase[sym] + in.bits(kLengthExtra[sym]);
            const int dsym = decode_symbol(in, dist);
            if (dsym >= 30) fail("距離符号が不正です");
            const std::size_t distance = kDistBase[dsym] + in.bits(kDistExtra[dsym]);
            if (distance > out.size()) fail("距離が出力の先頭を越えています");
            if (limit != 0 && out.size() + length > limit) fail("展開後の大きさが想定を超えました");
            const std::size_t from = out.size() - distance;
            for (unsigned i = 0; i < length; ++i) out.push_back(out[from + i]);
        }
    }
}

}  // namespace

std::vector<std::uint8_t> inflate_raw(const std::uint8_t* data, std::size_t size,
                                      std::size_t expected_size) {
    std::vector<std::uint8_t> out;
    if (expected_size != 0) out.reserve(expected_size);
    BitReader in(data, size);

    Huffman fixed_lit, fixed_dist;
    {
        unsigned char lengths[288];
        for (int i = 0; i < 144; ++i) lengths[i] = 8;
        for (int i = 144; i < 256; ++i) lengths[i] = 9;
        for (int i = 256; i < 280; ++i) lengths[i] = 7;
        for (int i = 280; i < 288; ++i) lengths[i] = 8;
        build_huffman(fixed_lit, lengths, 288);
        for (int i = 0; i < 30; ++i) lengths[i] = 5;
        build_huffman(fixed_dist, lengths, 30);
    }

    bool last = false;
    while (!last) {
        last = in.bits(1) != 0;
        const unsigned type = in.bits(2);
        if (type == 0) {
            in.align_to_byte();
            const unsigned len = in.byte() | (static_cast<unsigned>(in.byte()) << 8);
            const unsigned nlen = in.byte() | (static_cast<unsigned>(in.byte()) << 8);
            if ((len ^ 0xFFFFu) != nlen) fail("無圧縮ブロックの長さが壊れています");
            in.copy_bytes(out, len, expected_size);
        } else if (type == 1) {
            inflate_block(in, out, fixed_lit, fixed_dist, expected_size);
        } else if (type == 2) {
            const int nlen = static_cast<int>(in.bits(5)) + 257;
            const int ndist = static_cast<int>(in.bits(5)) + 1;
            const int ncode = static_cast<int>(in.bits(4)) + 4;
            if (nlen > 286 || ndist > 30) fail("動的ハフマン表の大きさが不正です");
            static const int order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                          11, 4,  12, 3, 13, 2, 14, 1, 15};
            unsigned char lengths[320] = {0};
            for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<unsigned char>(in.bits(3));
            Huffman code_huff;
            build_huffman(code_huff, lengths, 19);

            unsigned char all[320] = {0};
            int index = 0;
            while (index < nlen + ndist) {
                const int sym = decode_symbol(in, code_huff);
                if (sym < 16) {
                    all[index++] = static_cast<unsigned char>(sym);
                } else {
                    int repeat = 0;
                    unsigned char value = 0;
                    if (sym == 16) {
                        if (index == 0) fail("繰り返しの元になる符号長がありません");
                        value = all[index - 1];
                        repeat = 3 + static_cast<int>(in.bits(2));
                    } else if (sym == 17) {
                        repeat = 3 + static_cast<int>(in.bits(3));
                    } else {
                        repeat = 11 + static_cast<int>(in.bits(7));
                    }
                    if (index + repeat > nlen + ndist) fail("符号長の繰り返しが範囲を超えています");
                    while (repeat-- > 0) all[index++] = value;
                }
            }
            if (all[256] == 0) fail("ブロック終端の符号がありません");
            Huffman lit, dist;
            build_huffman(lit, all, nlen);
            build_huffman(dist, all + nlen, ndist);
            inflate_block(in, out, lit, dist, expected_size);
        } else {
            fail("未定義のブロック形式です");
        }
    }
    return out;
}

std::vector<std::uint8_t> inflate_zlib(const std::uint8_t* data, std::size_t size,
                                       std::size_t expected_size) {
    if (size < 6) fail("zlibストリームが短すぎます");
    const unsigned cmf = data[0], flg = data[1];
    if ((cmf & 0x0F) != 8 || ((cmf << 8) | flg) % 31 != 0) fail("zlibヘッダが不正です");
    if (flg & 0x20) fail("プリセット辞書付きのzlibには対応していません");
    std::vector<std::uint8_t> out = inflate_raw(data + 2, size - 2, expected_size);

    // 末尾の Adler-32 を確かめる。DEFLATEのビット列の終わりはバイト境界に揃わないため、
    // ストリームの最後の4バイトを読む（PNGのIDAT連結でも末尾は必ずAdler-32）。
    // 5552バイトごとに剰余を取る（その間は32bitで溢れないことが保証されている）。
    std::uint32_t s1 = 1, s2 = 0;
    std::size_t i = 0;
    while (i < out.size()) {
        const std::size_t end = std::min(out.size(), i + 5552);
        for (; i < end; ++i) {
            s1 += out[i];
            s2 += s1;
        }
        s1 %= 65521u;
        s2 %= 65521u;
    }
    const std::uint32_t expected = (s2 << 16) | s1;
    const std::uint8_t* tail = data + size - 4;
    const std::uint32_t stored = (static_cast<std::uint32_t>(tail[0]) << 24) |
                                 (static_cast<std::uint32_t>(tail[1]) << 16) |
                                 (static_cast<std::uint32_t>(tail[2]) << 8) | tail[3];
    if (stored != expected) fail("Adler-32 が一致しません（データが壊れています）");
    return out;
}

}  // namespace stackcore
