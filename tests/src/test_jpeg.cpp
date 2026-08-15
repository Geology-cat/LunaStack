#include <algorithm>
#include <cstdint>
#include <vector>

#include "microtest.hpp"
#include "stackcore/frame_buffer.hpp"
#include "stackcore/jpeg_decoder.hpp"

using stackcore::FrameBuffer;

namespace {

void marker(std::vector<std::uint8_t>& out, int value) {
    out.push_back(0xFF);
    out.push_back(static_cast<std::uint8_t>(value));
}

void be16(std::vector<std::uint8_t>& out, int value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

// DC=0 / AC=EOBだけを持つ、規格上有効な最小Huffman表。
void append_minimal_tables(std::vector<std::uint8_t>& out, bool with_ac_coefficient) {
    marker(out, 0xC4);
    // DC表18バイト。AC表は通常18、AC係数テスト時は19バイト。
    be16(out, with_ac_coefficient ? 39 : 38);

    out.push_back(0x00);  // DC table 0
    out.push_back(1);     // 長さ1の符号が1個
    for (int i = 1; i < 16; ++i) out.push_back(0);
    out.push_back(0);  // category 0

    out.push_back(0x10);  // AC table 0
    if (with_ac_coefficient) {
        out.push_back(1);  // code 0: run=0, size=4
        out.push_back(1);  // code 10: EOB
        for (int i = 2; i < 16; ++i) out.push_back(0);
        out.push_back(0x04);
        out.push_back(0x00);
    } else {
        out.push_back(1);  // code 0: EOB
        for (int i = 1; i < 16; ++i) out.push_back(0);
        out.push_back(0x00);
    }
}

std::vector<std::uint8_t> minimal_jpeg(int width, int height, bool color,
                                       bool subsampled = false, bool ac_coefficient = false,
                                       bool restart = false) {
    std::vector<std::uint8_t> out;
    marker(out, 0xD8);

    marker(out, 0xDB);
    be16(out, 67);
    out.push_back(0);  // 8bit / table 0
    for (int i = 0; i < 64; ++i) out.push_back(1);

    const int components = color ? 3 : 1;
    marker(out, 0xC0);
    be16(out, 8 + components * 3);
    out.push_back(8);
    be16(out, height);
    be16(out, width);
    out.push_back(static_cast<std::uint8_t>(components));
    for (int c = 0; c < components; ++c) {
        out.push_back(static_cast<std::uint8_t>(c + 1));
        out.push_back(static_cast<std::uint8_t>(subsampled && c == 0 ? 0x22 : 0x11));
        out.push_back(0);
    }

    append_minimal_tables(out, ac_coefficient);
    if (restart) {
        marker(out, 0xDD);
        be16(out, 4);
        be16(out, 1);
    }

    marker(out, 0xDA);
    be16(out, 6 + components * 2);
    out.push_back(static_cast<std::uint8_t>(components));
    for (int c = 0; c < components; ++c) {
        out.push_back(static_cast<std::uint8_t>(c + 1));
        out.push_back(0);
    }
    out.push_back(0);
    out.push_back(63);
    out.push_back(0);

    if (restart) {
        // 各MCUは DC code 0 + AC EOB code 0。残りを1で埋める。
        out.push_back(0x3F);
        marker(out, 0xD0);
        out.push_back(0x3F);
    } else if (ac_coefficient) {
        // DC 0 / AC(0,4) 0 / value 1111 / EOB 10 = 00111110。
        out.push_back(0x3E);
    } else if (components == 1) {
        out.push_back(0x3F);
    } else if (subsampled) {
        // Y 4ブロック + Cb + Cr = 12ゼロビット。
        out.push_back(0x00);
        out.push_back(0x0F);
    } else {
        // 3ブロック = 6ゼロビット。
        out.push_back(0x03);
    }
    marker(out, 0xD9);
    return out;
}

void check_constant(const FrameBuffer& frame, int channels) {
    MT_CHECK_EQ(frame.channels(), channels);
    for (int c = 0; c < channels; ++c) {
        for (int y = 0; y < frame.height(); ++y) {
            for (int x = 0; x < frame.width(); ++x) {
                MT_CHECK_NEAR(frame.row(c, y)[x], 128.0 / 255.0, 1e-7);
            }
        }
    }
}

}  // namespace

MT_TEST(jpeg_最小グレースケールを展開する) {
    const std::vector<std::uint8_t> bytes = minimal_jpeg(8, 8, false);
    FrameBuffer frame;
    stackcore::decode_baseline_jpeg(bytes.data(), bytes.size(), frame);
    MT_CHECK_EQ(frame.width(), 8);
    MT_CHECK_EQ(frame.height(), 8);
    check_constant(frame, 1);
}

MT_TEST(jpeg_三成分YCbCrをRGBへ展開する) {
    const std::vector<std::uint8_t> bytes = minimal_jpeg(8, 8, true);
    FrameBuffer frame;
    stackcore::decode_baseline_jpeg(bytes.data(), bytes.size(), frame);
    check_constant(frame, 3);
}

MT_TEST(jpeg_420サブサンプリングを展開する) {
    const std::vector<std::uint8_t> bytes = minimal_jpeg(16, 16, true, true);
    FrameBuffer frame;
    stackcore::decode_baseline_jpeg(bytes.data(), bytes.size(), frame);
    MT_CHECK_EQ(frame.width(), 16);
    MT_CHECK_EQ(frame.height(), 16);
    check_constant(frame, 3);
}

MT_TEST(jpeg_AC係数を固定小数点IDCTで展開する) {
    const std::vector<std::uint8_t> bytes = minimal_jpeg(8, 8, false, false, true);
    FrameBuffer frame;
    stackcore::decode_baseline_jpeg(bytes.data(), bytes.size(), frame);
    float lo = frame.row(0, 0)[0];
    float hi = lo;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            lo = std::min(lo, frame.row(0, y)[x]);
            hi = std::max(hi, frame.row(0, y)[x]);
        }
    }
    MT_CHECK(hi > lo);
}

MT_TEST(jpeg_再スタートマーカーを処理する) {
    const std::vector<std::uint8_t> bytes = minimal_jpeg(16, 8, false, false, false, true);
    FrameBuffer frame;
    stackcore::decode_baseline_jpeg(bytes.data(), bytes.size(), frame);
    MT_CHECK_EQ(frame.width(), 16);
    check_constant(frame, 1);
}

MT_TEST(jpeg_DHT省略時は規格の既定表を使う) {
    std::vector<std::uint8_t> bytes = minimal_jpeg(8, 8, false);
    for (std::size_t i = 0; i + 3 < bytes.size(); ++i) {
        if (bytes[i] != 0xFF || bytes[i + 1] != 0xC4) continue;
        const std::size_t length = (static_cast<std::size_t>(bytes[i + 2]) << 8) | bytes[i + 3];
        bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(i),
                    bytes.begin() + static_cast<std::ptrdiff_t>(i + 2 + length));
        break;
    }
    // 既定表ではDC category 0が00、AC EOBが1010。
    for (std::size_t i = 0; i + 9 < bytes.size(); ++i) {
        if (bytes[i] == 0xFF && bytes[i + 1] == 0xDA) {
            const std::size_t length = (static_cast<std::size_t>(bytes[i + 2]) << 8) | bytes[i + 3];
            bytes[i + 2 + length] = 0x2B;
            break;
        }
    }
    FrameBuffer frame;
    stackcore::decode_baseline_jpeg(bytes.data(), bytes.size(), frame);
    check_constant(frame, 1);
}

MT_TEST(jpeg_progressiveは明示的に拒否する) {
    std::vector<std::uint8_t> bytes = minimal_jpeg(8, 8, false);
    for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
        if (bytes[i] == 0xFF && bytes[i + 1] == 0xC0) {
            bytes[i + 1] = 0xC2;
            break;
        }
    }
    FrameBuffer frame;
    MT_CHECK_THROWS(stackcore::decode_baseline_jpeg(bytes.data(), bytes.size(), frame));
}

// AVI統合テストから同じ最小JPEGを使う。
std::vector<std::uint8_t> lunastack_test_minimal_color_jpeg() {
    return minimal_jpeg(8, 8, true);
}

std::vector<std::uint8_t> lunastack_test_minimal_gray_jpeg() {
    return minimal_jpeg(8, 8, false);
}
