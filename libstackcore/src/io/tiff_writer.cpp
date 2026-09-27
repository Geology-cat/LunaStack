#include "stackcore/tiff_writer.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <stdexcept>
#include <vector>

namespace stackcore {
namespace {

void put16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

struct IfdEntry {
    std::uint16_t tag;
    std::uint16_t type;   // 3 = SHORT, 4 = LONG
    std::uint32_t count;
    std::uint32_t value;  // 4バイトに収まらない場合はファイル内オフセット
};


}  // namespace

void write_tiff(const std::string& path, const FrameBuffer& image, TiffFormat format) {
    write_tiff(path, image, format, ImageMetadata());
}

void write_tiff(const std::string& path, const FrameBuffer& image, TiffFormat format,
                const ImageMetadata& metadata) {
    if (image.empty()) {
        throw std::invalid_argument("TIFF: 空の画像は書き出せません");
    }
    const int w = image.width();
    const int h = image.height();
    const int n = image.channels();
    if (n != 1 && n != 3) {
        throw std::invalid_argument("TIFF: 1ch または 3ch のみ対応しています (指定: " +
                                    std::to_string(n) + "ch)");
    }

    const bool is_uint16 = (format == TiffFormat::UInt16);
    const std::uint32_t sample_bytes = is_uint16 ? 2u : 4u;
    const std::uint16_t bits_per_sample = is_uint16 ? 16 : 32;
    const std::uint16_t sample_format = is_uint16 ? 1 : 3;  // 1=符号なし整数, 3=IEEE float

    const std::size_t data_bytes = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) *
                                   static_cast<std::size_t>(n) * sample_bytes;

    // TIFF（BigTIFFでない）はファイル内の位置を32bitで持つので、4GBを超えると位置があふれて
    // 壊れたファイルを黙って書いてしまう。タグ・メタデータの分も見込んで手前で断る。
    if (data_bytes > 0xFFFFFFFFull - (1ull << 20)) {
        throw std::runtime_error("TIFF: 4GBを超える画像は書き出せません（32bit float FITSで書き出してください）");
    }

    // 平面（planar）配置の float から、TIFFのインターリーブ配置へ変換する。
    std::vector<std::uint8_t> pixels(data_bytes);
    std::vector<const float*> rows(static_cast<std::size_t>(n));
    std::size_t o = 0;
    for (int y = 0; y < h; ++y) {
        for (int c = 0; c < n; ++c) rows[static_cast<std::size_t>(c)] = image.row(c, y);
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < n; ++c) {
                const float v = rows[static_cast<std::size_t>(c)][x];
                if (is_uint16) {
                    // NaN は比較が偽になるので 0 にする（整数への変換で未定義動作にしない）。
                    const float clamped = v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f;
                    const std::uint16_t u =
                        static_cast<std::uint16_t>(clamped * 65535.0f + 0.5f);
                    pixels[o++] = static_cast<std::uint8_t>(u & 0xFF);
                    pixels[o++] = static_cast<std::uint8_t>((u >> 8) & 0xFF);
                } else {
                    std::uint32_t raw;
                    std::memcpy(&raw, &v, sizeof(raw));
                    pixels[o++] = static_cast<std::uint8_t>(raw & 0xFF);
                    pixels[o++] = static_cast<std::uint8_t>((raw >> 8) & 0xFF);
                    pixels[o++] = static_cast<std::uint8_t>((raw >> 16) & 0xFF);
                    pixels[o++] = static_cast<std::uint8_t>((raw >> 24) & 0xFF);
                }
            }
        }
    }

    // 付加する文字列タグ（ASCII、NUL終端）。空のメタデータなら何も足さない。
    std::string description = metadata.description;
    for (const std::string& line : metadata.history) {
        if (!description.empty()) description += "\n";
        description += line;
    }
    std::string datetime;
    if (!metadata.date_obs.empty() && metadata.date_obs.size() >= 19) {
        // "YYYY-MM-DDTHH:MM:SS" → "YYYY:MM:DD HH:MM:SS"
        datetime = metadata.date_obs.substr(0, 19);
        datetime[4] = ':';
        datetime[7] = ':';
        datetime[10] = ' ';
    }
    const int num_entries = 11 + (description.empty() ? 0 : 1) +
                            (metadata.software.empty() ? 0 : 1) + (datetime.empty() ? 0 : 1);

    const std::uint32_t data_offset = 8;
    const std::uint32_t ifd_offset = data_offset + static_cast<std::uint32_t>(data_bytes);
    const std::uint32_t extra_offset =
        ifd_offset + 2u + 12u * static_cast<std::uint32_t>(num_entries) + 4u;

    // SHORT が3個だと4バイトのvalueフィールドに収まらないためIFDの後ろに置く。
    std::vector<std::uint8_t> extra;
    std::uint32_t bits_value;
    std::uint32_t format_value;
    if (n == 1) {
        bits_value = bits_per_sample;
        format_value = sample_format;
    } else {
        bits_value = extra_offset;
        for (int i = 0; i < n; ++i) put16(extra, bits_per_sample);
        format_value = extra_offset + static_cast<std::uint32_t>(extra.size());
        for (int i = 0; i < n; ++i) put16(extra, sample_format);
    }
    // ASCIIの値を置き、IFDエントリを返す。4バイト以下ならエントリ内に直接入れる。
    const auto ascii_entry = [&](std::uint16_t tag, const std::string& text) {
        IfdEntry e{tag, 2, static_cast<std::uint32_t>(text.size() + 1), 0};
        if (text.size() + 1 <= 4) {
            std::uint32_t v = 0;
            for (std::size_t i = 0; i < text.size(); ++i) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(text[i])) << (8 * i);
            e.value = v;
        } else {
            e.value = extra_offset + static_cast<std::uint32_t>(extra.size());
            extra.insert(extra.end(), text.begin(), text.end());
            extra.push_back(0);
            if (extra.size() % 2) extra.push_back(0);  // 値の位置は偶数に揃える（TIFFの推奨）
        }
        return e;
    };

    // タグは昇順に並べる必要がある。
    std::vector<IfdEntry> entries = {
        {256, 4, 1, static_cast<std::uint32_t>(w)},                  // ImageWidth
        {257, 4, 1, static_cast<std::uint32_t>(h)},                  // ImageLength
        {258, 3, static_cast<std::uint32_t>(n), bits_value},         // BitsPerSample
        {259, 3, 1, 1},                                              // Compression = なし
        {262, 3, 1, static_cast<std::uint32_t>(n == 1 ? 1 : 2)},     // Photometric
    };
    if (!description.empty()) entries.push_back(ascii_entry(270, description));  // ImageDescription
    entries.push_back({273, 4, 1, data_offset});                                 // StripOffsets
    entries.push_back({277, 3, 1, static_cast<std::uint32_t>(n)});               // SamplesPerPixel
    entries.push_back({278, 4, 1, static_cast<std::uint32_t>(h)});               // RowsPerStrip
    entries.push_back({279, 4, 1, static_cast<std::uint32_t>(data_bytes)});      // StripByteCounts
    entries.push_back({284, 3, 1, 1});                                           // PlanarConfiguration
    if (!metadata.software.empty()) entries.push_back(ascii_entry(305, metadata.software));
    if (!datetime.empty()) entries.push_back(ascii_entry(306, datetime));        // DateTime
    entries.push_back({339, 3, static_cast<std::uint32_t>(n), format_value});    // SampleFormat

    std::vector<std::uint8_t> out;
    out.reserve(8 + data_bytes + 2 + 12 * entries.size() + 4 + extra.size());
    out.push_back('I');
    out.push_back('I');
    put16(out, 42);
    put32(out, ifd_offset);
    out.insert(out.end(), pixels.begin(), pixels.end());
    put16(out, static_cast<std::uint16_t>(entries.size()));
    for (const IfdEntry& e : entries) {
        put16(out, e.tag);
        put16(out, e.type);
        put32(out, e.count);
        put32(out, e.value);
    }
    put32(out, 0);  // 次のIFDなし
    out.insert(out.end(), extra.begin(), extra.end());

    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (fp == nullptr) {
        throw std::runtime_error("TIFF: ファイルを作成できません: " + path);
    }
    const std::size_t written = std::fwrite(out.data(), 1, out.size(), fp);
    const int close_result = std::fclose(fp);
    if (written != out.size() || close_result != 0) {
        throw std::runtime_error("TIFF: 書き込みに失敗しました: " + path);
    }
}

}  // namespace stackcore
