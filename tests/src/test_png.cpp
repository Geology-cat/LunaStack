// PNG書き出しのテスト。IDATの無圧縮DEFLATEを自前で展開し、
// ヘッダ・複数ブロック・画素値・決定論性を検証する。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "microtest.hpp"
#include "stackcore/frame_buffer.hpp"
#include "stackcore/png_writer.hpp"

using stackcore::FrameBuffer;

namespace {

std::vector<std::uint8_t> read_all(const std::string& path) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) return std::vector<std::uint8_t>();
    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    std::vector<std::uint8_t> out(size > 0 ? static_cast<std::size_t>(size) : 0);
    if (!out.empty()) out.resize(std::fread(out.data(), 1, out.size(), fp));
    std::fclose(fp);
    return out;
}

std::uint32_t be32(const std::vector<std::uint8_t>& b, std::size_t o) {
    return (static_cast<std::uint32_t>(b[o]) << 24) |
           (static_cast<std::uint32_t>(b[o + 1]) << 16) |
           (static_cast<std::uint32_t>(b[o + 2]) << 8) |
           static_cast<std::uint32_t>(b[o + 3]);
}

struct ParsedPng {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t bit_depth = 0;
    std::uint8_t color_type = 0;
    int idat_chunks = 0;
    std::vector<std::uint8_t> raw;
};

ParsedPng parse_png(const std::vector<std::uint8_t>& bytes) {
    const std::uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (bytes.size() < 8 || std::memcmp(bytes.data(), signature, 8) != 0) {
        MT_FAIL_AT(std::string("PNGシグネチャが不正です"));
    }

    ParsedPng png;
    std::vector<std::uint8_t> zlib;
    std::size_t pos = 8;
    while (pos + 12 <= bytes.size()) {
        const std::uint32_t length = be32(bytes, pos);
        if (pos + 12u + length > bytes.size()) MT_FAIL_AT(std::string("PNGチャンクが途切れています"));
        const char* type = reinterpret_cast<const char*>(bytes.data() + pos + 4);
        const std::size_t data = pos + 8;
        if (std::memcmp(type, "IHDR", 4) == 0) {
            if (length != 13) MT_FAIL_AT(std::string("IHDR長が不正です"));
            png.width = be32(bytes, data);
            png.height = be32(bytes, data + 4);
            png.bit_depth = bytes[data + 8];
            png.color_type = bytes[data + 9];
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            ++png.idat_chunks;
            zlib.insert(zlib.end(), bytes.begin() + static_cast<std::ptrdiff_t>(data),
                        bytes.begin() + static_cast<std::ptrdiff_t>(data + length));
        }
        pos += 12u + length;
    }

    if (zlib.size() < 7 || zlib[0] != 0x78 || zlib[1] != 0x01) {
        MT_FAIL_AT(std::string("zlibヘッダが不正です"));
    }
    pos = 2;
    bool final = false;
    while (!final) {
        if (pos + 5 > zlib.size()) MT_FAIL_AT(std::string("DEFLATEブロックが途切れています"));
        const std::uint8_t header = zlib[pos++];
        final = (header & 1u) != 0;
        if ((header & 6u) != 0) MT_FAIL_AT(std::string("無圧縮DEFLATEではありません"));
        const std::uint16_t len = static_cast<std::uint16_t>(zlib[pos] | (zlib[pos + 1] << 8));
        const std::uint16_t nlen =
            static_cast<std::uint16_t>(zlib[pos + 2] | (zlib[pos + 3] << 8));
        pos += 4;
        if (static_cast<std::uint16_t>(~len) != nlen || pos + len > zlib.size()) {
            MT_FAIL_AT(std::string("DEFLATEのLEN/NLENが不正です"));
        }
        png.raw.insert(png.raw.end(), zlib.begin() + static_cast<std::ptrdiff_t>(pos),
                       zlib.begin() + static_cast<std::ptrdiff_t>(pos + len));
        pos += len;
    }
    if (pos + 4 != zlib.size()) MT_FAIL_AT(std::string("zlib末尾長が不正です"));
    return png;
}

std::string temp_path(const char* name) { return std::string("/tmp/lunastack_test_") + name; }

}  // namespace

MT_TEST(png_16bitRGBの構造と画素値) {
    FrameBuffer image(3, 2, 3);
    image.row(0, 0)[0] = 1.0f;
    image.row(1, 0)[0] = 0.5f;
    image.row(2, 0)[0] = 0.0f;
    image.row(0, 1)[2] = 0.25f;

    const std::string path = temp_path("rgb16.png");
    stackcore::write_png16(path, image);
    const ParsedPng png = parse_png(read_all(path));

    MT_CHECK_EQ(png.width, 3u);
    MT_CHECK_EQ(png.height, 2u);
    MT_CHECK_EQ(png.bit_depth, 16u);
    MT_CHECK_EQ(png.color_type, 2u);
    MT_CHECK_EQ(png.raw.size(), static_cast<std::size_t>(2 * (1 + 3 * 3 * 2)));
    MT_CHECK_EQ(png.raw[0], 0u);  // filter None
    MT_CHECK_EQ(png.raw[1], 0xffu);
    MT_CHECK_EQ(png.raw[2], 0xffu);
    MT_CHECK_EQ(png.raw[3], 0x80u);
    MT_CHECK_EQ(png.raw[4], 0x00u);
}

MT_TEST(png_大きい画像は複数IDATでも読み戻せて決定論的) {
    FrameBuffer image(200, 200, 3);
    for (int c = 0; c < 3; ++c) {
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                image.row(c, y)[x] = static_cast<float>((x + y + c) % 256) / 255.0f;
            }
        }
    }

    const std::string a = temp_path("multi_a.png");
    const std::string b = temp_path("multi_b.png");
    stackcore::write_png16(a, image);
    stackcore::write_png16(b, image);
    const std::vector<std::uint8_t> bytes_a = read_all(a);
    const std::vector<std::uint8_t> bytes_b = read_all(b);
    MT_CHECK(bytes_a == bytes_b);
    const ParsedPng png = parse_png(bytes_a);
    MT_CHECK(png.idat_chunks > 1);
    MT_CHECK_EQ(png.raw.size(), static_cast<std::size_t>(200 * (1 + 200 * 3 * 2)));
}

MT_TEST(png_値はクランプされ未対応チャンネルは例外) {
    FrameBuffer gray(2, 1, 1);
    gray.row(0, 0)[0] = -1.0f;
    gray.row(0, 0)[1] = 2.0f;
    const std::string path = temp_path("clamp.png");
    stackcore::write_png16(path, gray);
    const ParsedPng png = parse_png(read_all(path));
    MT_CHECK_EQ(png.color_type, 0u);
    MT_CHECK_EQ(png.raw[1], 0u);
    MT_CHECK_EQ(png.raw[2], 0u);
    MT_CHECK_EQ(png.raw[3], 0xffu);
    MT_CHECK_EQ(png.raw[4], 0xffu);

    FrameBuffer bad(2, 2, 2);
    MT_CHECK_THROWS(stackcore::write_png16(temp_path("bad.png"), bad));
    FrameBuffer empty;
    MT_CHECK_THROWS(stackcore::write_png16(temp_path("empty.png"), empty));
}
