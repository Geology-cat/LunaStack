// 32bit float FITS書き出しの構造・値・RGB面順を検証する。

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "microtest.hpp"
#include "stackcore/fits_writer.hpp"
#include "stackcore/frame_buffer.hpp"

using stackcore::FrameBuffer;

namespace {

std::string temp_path(const char* name) { return std::string("/tmp/lunastack_test_") + name; }

std::vector<unsigned char> read_all(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return {};
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    std::vector<unsigned char> bytes(size > 0 ? static_cast<std::size_t>(size) : 0);
    if (!bytes.empty()) {
        const std::size_t got = std::fread(bytes.data(), 1, bytes.size(), file);
        bytes.resize(got);
    }
    std::fclose(file);
    return bytes;
}

std::string card_at(const std::vector<unsigned char>& bytes, std::size_t index) {
    const std::size_t offset = index * 80;
    if (offset + 80 > bytes.size()) return {};
    return std::string(reinterpret_cast<const char*>(bytes.data() + offset), 80);
}

std::string card_value(const std::vector<unsigned char>& bytes, const std::string& keyword) {
    for (std::size_t i = 0; i * 80 + 80 <= bytes.size(); ++i) {
        const std::string card = card_at(bytes, i);
        if (card.compare(0, keyword.size(), keyword) == 0) {
            if (card.size() < 30 || card[8] != '=') return {};
            std::string value = card.substr(10, 20);
            const std::size_t first = value.find_first_not_of(' ');
            const std::size_t last = value.find_last_not_of(' ');
            return first == std::string::npos ? std::string() : value.substr(first, last - first + 1);
        }
        if (card.compare(0, 3, "END") == 0) break;
    }
    return {};
}

float be_float(const std::vector<unsigned char>& bytes, std::size_t offset) {
    const std::uint32_t raw = (static_cast<std::uint32_t>(bytes[offset]) << 24) |
                              (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
                              (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
                              static_cast<std::uint32_t>(bytes[offset + 3]);
    float value = 0.0f;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

}  // namespace

MT_TEST(fits_32bitfloatグレースケールは標準PrimaryHDUになる) {
    FrameBuffer image(3, 2, 1);
    image.row(0, 0)[0] = 0.0f;
    image.row(0, 0)[1] = 0.5f;
    image.row(0, 0)[2] = 1.0f;
    image.row(0, 1)[0] = -0.5f;
    image.row(0, 1)[1] = 2.0f;
    image.row(0, 1)[2] = std::nanf("");

    const std::string path = temp_path("gray32.fits");
    stackcore::write_fits_float32(path, image);
    const std::vector<unsigned char> bytes = read_all(path);

    MT_CHECK_EQ(bytes.size() % 2880u, 0u);
    MT_CHECK_EQ(card_value(bytes, "SIMPLE"), std::string("T"));
    MT_CHECK_EQ(card_value(bytes, "BITPIX"), std::string("-32"));
    MT_CHECK_EQ(card_value(bytes, "NAXIS"), std::string("2"));
    MT_CHECK_EQ(card_value(bytes, "NAXIS1"), std::string("3"));
    MT_CHECK_EQ(card_value(bytes, "NAXIS2"), std::string("2"));

    const std::size_t data = 2880;
    MT_CHECK_NEAR(be_float(bytes, data + 0), 0.0, 1e-7);
    MT_CHECK_NEAR(be_float(bytes, data + 4), 0.5, 1e-7);
    MT_CHECK_NEAR(be_float(bytes, data + 8), 1.0, 1e-7);
    MT_CHECK_NEAR(be_float(bytes, data + 12), 0.0, 1e-7);  // 負値をクランプ
    MT_CHECK_NEAR(be_float(bytes, data + 16), 1.0, 1e-7);  // 1超をクランプ
    MT_CHECK_NEAR(be_float(bytes, data + 20), 0.0, 1e-7);  // NaNを0へ
}

MT_TEST(fits_RGBは単一HDUの3次元データキューブになる) {
    FrameBuffer image(2, 1, 3);
    image.row(0, 0)[0] = 0.1f;
    image.row(0, 0)[1] = 0.2f;
    image.row(1, 0)[0] = 0.3f;
    image.row(1, 0)[1] = 0.4f;
    image.row(2, 0)[0] = 0.5f;
    image.row(2, 0)[1] = 0.6f;

    const std::string path = temp_path("rgb32.fits");
    stackcore::write_fits_float32(path, image);
    const std::vector<unsigned char> bytes = read_all(path);
    MT_CHECK_EQ(card_value(bytes, "NAXIS"), std::string("3"));
    MT_CHECK_EQ(card_value(bytes, "NAXIS1"), std::string("2"));
    MT_CHECK_EQ(card_value(bytes, "NAXIS2"), std::string("1"));
    MT_CHECK_EQ(card_value(bytes, "NAXIS3"), std::string("3"));

    const std::size_t data = 2880;
    const double expected[] = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6};
    for (std::size_t i = 0; i < 6; ++i) {
        MT_CHECK_NEAR(be_float(bytes, data + i * 4), expected[i], 1e-6);
    }
}

MT_TEST(fits_空画像と未対応チャンネル数を拒否する) {
    FrameBuffer empty;
    MT_CHECK_THROWS(stackcore::write_fits_float32(temp_path("empty.fits"), empty));
    FrameBuffer two_channels(2, 2, 2);
    MT_CHECK_THROWS(
        stackcore::write_fits_float32(temp_path("two_channels.fits"), two_channels));
}
