#include "stackcore/png_writer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <stdexcept>
#include <vector>

namespace stackcore {
namespace {

void put_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
    out.push_back(static_cast<std::uint8_t>(value & 0xffu));
}

std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t* data, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return crc;
}

bool write_all(std::FILE* fp, const void* data, std::size_t size) {
    return size == 0 || std::fwrite(data, 1, size, fp) == size;
}

void write_chunk(std::FILE* fp, const char type[4], const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> length;
    put_be32(length, static_cast<std::uint32_t>(data.size()));

    std::uint32_t crc = 0xffffffffu;
    crc = crc32_update(crc, reinterpret_cast<const std::uint8_t*>(type), 4);
    crc = crc32_update(crc, data.data(), data.size()) ^ 0xffffffffu;
    std::vector<std::uint8_t> crc_bytes;
    put_be32(crc_bytes, crc);

    if (!write_all(fp, length.data(), length.size()) || !write_all(fp, type, 4) ||
        !write_all(fp, data.data(), data.size()) ||
        !write_all(fp, crc_bytes.data(), crc_bytes.size())) {
        throw std::runtime_error("PNG: 書き込みに失敗しました");
    }
}

struct Adler32 {
    std::uint32_t s1 = 1;
    std::uint32_t s2 = 0;
    std::size_t pending = 0;

    void update(std::uint8_t byte) {
        s1 += byte;
        s2 += s1;
        if (++pending == 5552) {
            s1 %= 65521u;
            s2 %= 65521u;
            pending = 0;
        }
    }

    std::uint32_t value() const { return ((s2 % 65521u) << 16) | (s1 % 65521u); }
};

}  // namespace

void write_png16(const std::string& path, const FrameBuffer& image) {
    write_png16(path, image, ImageMetadata());
}

void write_png16(const std::string& path, const FrameBuffer& image,
                 const ImageMetadata& metadata) {
    if (image.empty()) throw std::invalid_argument("PNG: 空の画像は書き出せません");
    const int width = image.width();
    const int height = image.height();
    const int channels = image.channels();
    if (channels != 1 && channels != 3) {
        throw std::invalid_argument("PNG: 1ch または 3ch のみ対応しています (指定: " +
                                    std::to_string(channels) + "ch)");
    }

    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (fp == nullptr) throw std::runtime_error("PNG: ファイルを作成できません: " + path);

    try {
        const std::uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
        if (!write_all(fp, signature, sizeof(signature))) {
            throw std::runtime_error("PNG: 書き込みに失敗しました");
        }

        std::vector<std::uint8_t> ihdr;
        put_be32(ihdr, static_cast<std::uint32_t>(width));
        put_be32(ihdr, static_cast<std::uint32_t>(height));
        ihdr.push_back(16);                         // bit depth
        ihdr.push_back(channels == 1 ? 0 : 2);     // grayscale / truecolor
        ihdr.push_back(0);                          // compression
        ihdr.push_back(0);                          // filter
        ihdr.push_back(0);                          // interlace
        write_chunk(fp, "IHDR", ihdr);

        // tEXt は Latin-1 と定められているが、日本語を含みうる説明はUTF-8のまま
        // iTXt で書く。キーワードはPNG仕様の既定語を使う。
        const auto text_chunk = [&](const char* keyword, const std::string& text) {
            if (text.empty()) return;
            std::vector<std::uint8_t> body(keyword, keyword + std::strlen(keyword));
            body.push_back(0);  // キーワード終端
            body.push_back(0);  // 圧縮なし
            body.push_back(0);  // 圧縮方式
            body.push_back(0);  // 言語タグ（空）
            body.push_back(0);  // 翻訳キーワード（空）
            body.insert(body.end(), text.begin(), text.end());
            write_chunk(fp, "iTXt", body);
        };
        std::string description = metadata.description;
        for (const std::string& line : metadata.history) {
            if (!description.empty()) description += "\n";
            description += line;
        }
        text_chunk("Software", metadata.software);
        text_chunk("Description", description);
        text_chunk("Creation Time", metadata.date_obs);
        text_chunk("Title", metadata.object);

        const std::uint64_t row_bytes =
            1u + static_cast<std::uint64_t>(width) * channels * 2u;
        const std::uint64_t total_raw = row_bytes * static_cast<std::uint64_t>(height);
        std::uint64_t emitted = 0;
        bool first_block = true;
        Adler32 adler;
        std::vector<std::uint8_t> block;
        block.reserve(65535);

        const auto flush_block = [&](bool final) {
            if (block.empty()) return;
            std::vector<std::uint8_t> idat;
            idat.reserve(block.size() + 11);
            if (first_block) {
                idat.push_back(0x78);  // zlib CMF: DEFLATE, 32KB window
                idat.push_back(0x01);  // 最速/無圧縮。CMF+FLGは31の倍数
                first_block = false;
            }
            idat.push_back(final ? 0x01 : 0x00);  // BFINAL + stored block
            const std::uint16_t len = static_cast<std::uint16_t>(block.size());
            const std::uint16_t nlen = static_cast<std::uint16_t>(~len);
            idat.push_back(static_cast<std::uint8_t>(len & 0xffu));
            idat.push_back(static_cast<std::uint8_t>((len >> 8) & 0xffu));
            idat.push_back(static_cast<std::uint8_t>(nlen & 0xffu));
            idat.push_back(static_cast<std::uint8_t>((nlen >> 8) & 0xffu));
            idat.insert(idat.end(), block.begin(), block.end());
            if (final) put_be32(idat, adler.value());
            write_chunk(fp, "IDAT", idat);
            block.clear();
        };

        const auto append_byte = [&](std::uint8_t byte) {
            block.push_back(byte);
            adler.update(byte);
            ++emitted;
            if (block.size() == 65535 || emitted == total_raw) {
                flush_block(emitted == total_raw);
            }
        };

        for (int y = 0; y < height; ++y) {
            append_byte(0);  // PNG filter: None
            for (int x = 0; x < width; ++x) {
                for (int c = 0; c < channels; ++c) {
                    const float value = image.row(c, y)[x];
                    const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
                    const std::uint16_t sample =
                        static_cast<std::uint16_t>(clamped * 65535.0f + 0.5f);
                    append_byte(static_cast<std::uint8_t>((sample >> 8) & 0xffu));
                    append_byte(static_cast<std::uint8_t>(sample & 0xffu));
                }
            }
        }

        write_chunk(fp, "IEND", std::vector<std::uint8_t>());
        if (std::fclose(fp) != 0) {
            fp = nullptr;
            throw std::runtime_error("PNG: 書き込みに失敗しました: " + path);
        }
        fp = nullptr;
    } catch (...) {
        if (fp != nullptr) std::fclose(fp);
        throw;
    }
}

}  // namespace stackcore
