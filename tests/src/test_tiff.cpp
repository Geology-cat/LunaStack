// TIFF書き出しのテスト。
// 自作のパーサで読み返して構造と画素値を検証する。
// これは「TIFFとして妥当か」の一次確認であり、他アプリで開けることは
// ビルド後に外部ツール（sips 等）で別途確認する。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "microtest.hpp"
#include "stackcore/frame_buffer.hpp"
#include "stackcore/tiff_writer.hpp"

using stackcore::FrameBuffer;
using stackcore::TiffFormat;

namespace {

std::vector<std::uint8_t> read_all(const std::string& path) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) return std::vector<std::uint8_t>();
    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    std::vector<std::uint8_t> buf(size > 0 ? static_cast<std::size_t>(size) : 0);
    if (!buf.empty()) {
        const std::size_t got = std::fread(buf.data(), 1, buf.size(), fp);
        buf.resize(got);
    }
    std::fclose(fp);
    return buf;
}

std::uint16_t le16(const std::vector<std::uint8_t>& b, std::size_t o) {
    return static_cast<std::uint16_t>(b[o] | (b[o + 1] << 8));
}

std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t o) {
    return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8) |
           (static_cast<std::uint32_t>(b[o + 2]) << 16) |
           (static_cast<std::uint32_t>(b[o + 3]) << 24);
}

struct ParsedTiff {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t samples = 0;
    std::uint16_t bits = 0;
    std::uint16_t sample_format = 0;
    std::uint16_t photometric = 0;
    std::uint16_t compression = 0;
    std::uint32_t strip_offset = 0;
    std::uint32_t strip_bytes = 0;
    bool tags_ascending = true;
};

ParsedTiff parse_tiff(const std::vector<std::uint8_t>& b) {
    ParsedTiff t;
    if (b.size() < 8) MT_FAIL_AT(std::string("TIFFが短すぎます"));
    if (!(b[0] == 'I' && b[1] == 'I')) MT_FAIL_AT(std::string("リトルエンディアンのマジックではありません"));
    if (le16(b, 2) != 42) MT_FAIL_AT(std::string("TIFFのバージョン番号が42ではありません"));

    const std::uint32_t ifd = le32(b, 4);
    if (ifd + 2 > b.size()) MT_FAIL_AT(std::string("IFDオフセットがファイル外を指しています"));
    const std::uint16_t count = le16(b, ifd);

    std::uint32_t previous_tag = 0;
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::size_t e = ifd + 2 + static_cast<std::size_t>(i) * 12;
        if (e + 12 > b.size()) MT_FAIL_AT(std::string("IFDエントリがファイル外にあります"));
        const std::uint16_t tag = le16(b, e);
        const std::uint32_t n = le32(b, e + 4);
        const std::uint32_t value = le32(b, e + 8);

        if (tag < previous_tag) t.tags_ascending = false;
        previous_tag = tag;

        // 値が4バイトに収まらない場合はオフセット先の最初の要素を読む。
        const bool inline_value = (n * 2u) <= 4u;

        switch (tag) {
            case 256: t.width = value; break;
            case 257: t.height = value; break;
            case 258:
                t.bits = inline_value ? static_cast<std::uint16_t>(value & 0xFFFF)
                                      : le16(b, value);
                break;
            case 259: t.compression = static_cast<std::uint16_t>(value & 0xFFFF); break;
            case 262: t.photometric = static_cast<std::uint16_t>(value & 0xFFFF); break;
            case 273: t.strip_offset = value; break;
            case 277: t.samples = static_cast<std::uint16_t>(value & 0xFFFF); break;
            case 279: t.strip_bytes = value; break;
            case 339:
                t.sample_format = inline_value ? static_cast<std::uint16_t>(value & 0xFFFF)
                                               : le16(b, value);
                break;
            default: break;
        }
    }
    return t;
}

std::string temp_path(const char* name) { return std::string("./lunastack_test_") + name; }

}  // namespace

MT_TEST(tiff_16bitグレースケールの構造と画素値) {
    FrameBuffer fb(5, 3, 1);
    fb.row(0, 0)[0] = 0.0f;
    fb.row(0, 0)[1] = 1.0f;
    fb.row(0, 1)[2] = 0.5f;
    fb.row(0, 2)[4] = 0.25f;

    const std::string path = temp_path("gray16.tif");
    stackcore::write_tiff(path, fb, TiffFormat::UInt16);

    const std::vector<std::uint8_t> bytes = read_all(path);
    MT_CHECK(!bytes.empty());
    const ParsedTiff t = parse_tiff(bytes);

    MT_CHECK_EQ(t.width, 5u);
    MT_CHECK_EQ(t.height, 3u);
    MT_CHECK_EQ(t.samples, 1u);
    MT_CHECK_EQ(t.bits, 16u);
    MT_CHECK_EQ(t.sample_format, 1u);
    MT_CHECK_EQ(t.photometric, 1u);  // BlackIsZero
    MT_CHECK_EQ(t.compression, 1u);  // 非圧縮
    MT_CHECK_EQ(t.strip_bytes, 5u * 3u * 2u);
    MT_CHECK(t.tags_ascending);

    // 画素値を読み返す。
    const std::size_t base = t.strip_offset;
    MT_CHECK_EQ(le16(bytes, base + 0), 0u);          // (0,0) = 0.0
    MT_CHECK_EQ(le16(bytes, base + 2), 65535u);      // (1,0) = 1.0
    MT_CHECK_EQ(le16(bytes, base + (5 + 2) * 2), 32768u);  // (2,1) = 0.5
    MT_CHECK_EQ(le16(bytes, base + (10 + 4) * 2), 16384u); // (4,2) = 0.25
}

MT_TEST(tiff_値が0から1の範囲外でもクランプされる) {
    FrameBuffer fb(2, 1, 1);
    fb.row(0, 0)[0] = -5.0f;
    fb.row(0, 0)[1] = 7.0f;

    const std::string path = temp_path("clamp.tif");
    stackcore::write_tiff(path, fb, TiffFormat::UInt16);

    const std::vector<std::uint8_t> bytes = read_all(path);
    const ParsedTiff t = parse_tiff(bytes);
    MT_CHECK_EQ(le16(bytes, t.strip_offset + 0), 0u);
    MT_CHECK_EQ(le16(bytes, t.strip_offset + 2), 65535u);
}

MT_TEST(tiff_RGBはインターリーブされ配列タグがオフセット参照になる) {
    FrameBuffer fb(4, 2, 3);
    fb.row(0, 0)[0] = 1.0f;  // R
    fb.row(1, 0)[0] = 0.0f;  // G
    fb.row(2, 0)[0] = 0.5f;  // B

    const std::string path = temp_path("rgb16.tif");
    stackcore::write_tiff(path, fb, TiffFormat::UInt16);

    const std::vector<std::uint8_t> bytes = read_all(path);
    const ParsedTiff t = parse_tiff(bytes);

    MT_CHECK_EQ(t.samples, 3u);
    MT_CHECK_EQ(t.bits, 16u);          // オフセット先から読めていること
    MT_CHECK_EQ(t.sample_format, 1u);  // 同上
    MT_CHECK_EQ(t.photometric, 2u);    // RGB
    MT_CHECK_EQ(t.strip_bytes, 4u * 2u * 3u * 2u);

    // 先頭画素が R,G,B の順で並んでいること。
    MT_CHECK_EQ(le16(bytes, t.strip_offset + 0), 65535u);
    MT_CHECK_EQ(le16(bytes, t.strip_offset + 2), 0u);
    MT_CHECK_EQ(le16(bytes, t.strip_offset + 4), 32768u);
}

MT_TEST(tiff_32bitfloatは値をそのまま保持する) {
    FrameBuffer fb(3, 1, 1);
    fb.row(0, 0)[0] = 0.125f;
    fb.row(0, 0)[1] = 1.5f;   // 1.0超もクランプせず保持する
    fb.row(0, 0)[2] = -0.25f;

    const std::string path = temp_path("float32.tif");
    stackcore::write_tiff(path, fb, TiffFormat::Float32);

    const std::vector<std::uint8_t> bytes = read_all(path);
    const ParsedTiff t = parse_tiff(bytes);
    MT_CHECK_EQ(t.bits, 32u);
    MT_CHECK_EQ(t.sample_format, 3u);  // IEEE float
    MT_CHECK_EQ(t.strip_bytes, 3u * 4u);

    for (int i = 0; i < 3; ++i) {
        const std::uint32_t raw = le32(bytes, t.strip_offset + static_cast<std::size_t>(i) * 4);
        float v;
        std::memcpy(&v, &raw, sizeof(v));
        MT_CHECK_NEAR(v, fb.row(0, 0)[i], 1e-9);
    }
}

MT_TEST(tiff_未対応チャンネル数は例外になる) {
    FrameBuffer fb(2, 2, 2);
    MT_CHECK_THROWS(stackcore::write_tiff(temp_path("bad.tif"), fb, TiffFormat::UInt16));
}

MT_TEST(tiff_空の画像は例外になる) {
    FrameBuffer fb;
    MT_CHECK_THROWS(stackcore::write_tiff(temp_path("empty.tif"), fb, TiffFormat::UInt16));
}
