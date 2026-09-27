#include "stackcore/image_reader.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "stackcore/inflate.hpp"
#include "stackcore/jpeg_decoder.hpp"
#include "stackcore/raw_reader.hpp"

namespace stackcore {
namespace {

[[noreturn]] void fail(const std::string& format, const std::string& what) {
    throw std::runtime_error(format + ": " + what);
}

std::string lower_extension(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return ext;
}

std::vector<std::uint8_t> read_all(const std::string& path) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) throw std::runtime_error("画像を開けません: " + path);
    std::vector<std::uint8_t> data;
    if (std::fseek(fp, 0, SEEK_END) == 0) {
        const long size = std::ftell(fp);
        if (size > 0) data.resize(static_cast<std::size_t>(size));
        std::rewind(fp);
    }
    std::size_t got = data.empty() ? 0 : std::fread(data.data(), 1, data.size(), fp);
    data.resize(got);
    // 大きさが分からない（パイプ等）ときや途中で伸びたときに備えて、残りも読む。
    std::uint8_t buffer[1 << 16];
    for (;;) {
        const std::size_t n = std::fread(buffer, 1, sizeof(buffer), fp);
        data.insert(data.end(), buffer, buffer + n);
        if (n < sizeof(buffer)) break;
    }
    const bool error = std::ferror(fp) != 0;
    std::fclose(fp);
    if (error) throw std::runtime_error("画像を読めません: " + path);
    return data;
}

float clamp01(double v) {
    if (!std::isfinite(v)) return 0.0f;
    return static_cast<float>(v);
}

// 浮動小数点の画素を 0..1 に揃える。値が1を超えるなら16bitスケールとみなす。
void normalize_float_scale(FrameBuffer& out) {
    float maximum = 0.0f;
    for (int c = 0; c < out.channels(); ++c) {
        for (int y = 0; y < out.height(); ++y) {
            const float* row = out.row(c, y);
            for (int x = 0; x < out.width(); ++x) {
                if (std::isfinite(row[x]) && row[x] > maximum) maximum = row[x];
            }
        }
    }
    if (maximum <= 1.0f) return;
    const float scale = maximum <= 65535.0f * 1.01f ? 1.0f / 65535.0f : 1.0f / maximum;
    for (int c = 0; c < out.channels(); ++c) {
        for (int y = 0; y < out.height(); ++y) {
            float* row = out.row(c, y);
            for (int x = 0; x < out.width(); ++x) row[x] *= scale;
        }
    }
}

// 画像の寸法の上限。ヘッダの数値だけで確保量が決まるので、数十バイトの壊れたファイルでも
// 数十GBを確保して0で埋めにいき、Macがメモリを使い果たしてしまう。実在する天体写真
// （最大でも1億5千万画素ほど）より十分大きく、確保が現実的な範囲に収める。
void check_image_size(const char* format, std::int64_t w, std::int64_t h) {
    if (w <= 0 || h <= 0) fail(format, "画像の寸法がありません");
    if (w > 100000 || h > 100000 || w * h > 300000000) {
        fail(format, "画像が大きすぎます (" + std::to_string(w) + " x " + std::to_string(h) + ")");
    }
}

// ---- TIFF ------------------------------------------------------------------

struct TiffReader {
    const std::vector<std::uint8_t>& d;
    bool little = true;

    explicit TiffReader(const std::vector<std::uint8_t>& data) : d(data) {}

    void need(std::size_t offset, std::size_t size) const {
        if (offset > d.size() || size > d.size() - offset) fail("TIFF", "ファイルが途中で切れています");
    }
    std::uint16_t u16(std::size_t o) const {
        need(o, 2);
        return little ? static_cast<std::uint16_t>(d[o] | (d[o + 1] << 8))
                      : static_cast<std::uint16_t>((d[o] << 8) | d[o + 1]);
    }
    std::uint32_t u32(std::size_t o) const {
        need(o, 4);
        return little ? (static_cast<std::uint32_t>(d[o]) | (static_cast<std::uint32_t>(d[o + 1]) << 8) |
                         (static_cast<std::uint32_t>(d[o + 2]) << 16) |
                         (static_cast<std::uint32_t>(d[o + 3]) << 24))
                      : ((static_cast<std::uint32_t>(d[o]) << 24) |
                         (static_cast<std::uint32_t>(d[o + 1]) << 16) |
                         (static_cast<std::uint32_t>(d[o + 2]) << 8) | d[o + 3]);
    }
};

struct TiffTag {
    std::uint16_t type = 0;
    std::uint32_t count = 0;
    std::size_t value_offset = 0;  // 値そのものが置かれている位置
};

int tiff_type_size(std::uint16_t type) {
    switch (type) {
        case 1: case 2: case 6: case 7: return 1;
        case 3: case 8: return 2;
        case 4: case 9: case 11: return 4;
        case 5: case 10: case 12: return 8;
        default: return 0;
    }
}

std::vector<std::uint32_t> tiff_values(const TiffReader& r, const TiffTag& t) {
    std::vector<std::uint32_t> out;
    const int size = tiff_type_size(t.type);
    for (std::uint32_t i = 0; i < t.count; ++i) {
        const std::size_t o = t.value_offset + static_cast<std::size_t>(i) * size;
        if (t.type == 1 || t.type == 7) { r.need(o, 1); out.push_back(r.d[o]); }
        else if (t.type == 3) out.push_back(r.u16(o));
        else if (t.type == 4) out.push_back(r.u32(o));
        else fail("TIFF", "想定外のタグ型です");
    }
    return out;
}

// TIFF版LZW（MSBから詰める、コード幅は「早めに」切り替わる）。
std::vector<std::uint8_t> tiff_lzw_decode(const std::uint8_t* data, std::size_t size,
                                          std::size_t expected) {
    std::vector<std::uint8_t> out;
    out.reserve(expected);
    std::vector<std::vector<std::uint8_t>> table;
    const auto reset = [&]() {
        table.assign(258, std::vector<std::uint8_t>());
        for (int i = 0; i < 256; ++i) table[static_cast<std::size_t>(i)].assign(1, static_cast<std::uint8_t>(i));
    };
    reset();
    std::size_t bitpos = 0;
    int width = 9;
    int previous = -1;
    const std::size_t total_bits = size * 8;
    while (bitpos + static_cast<std::size_t>(width) <= total_bits) {
        unsigned code = 0;
        for (int b = 0; b < width; ++b, ++bitpos) {
            code = (code << 1) | ((data[bitpos >> 3] >> (7 - (bitpos & 7))) & 1u);
        }
        if (code == 257) break;  // EOI
        if (code == 256) {       // Clear
            reset();
            width = 9;
            previous = -1;
            continue;
        }
        std::vector<std::uint8_t> entry;
        if (code < table.size()) {
            entry = table[code];
            if (previous >= 0) {
                std::vector<std::uint8_t> added = table[static_cast<std::size_t>(previous)];
                added.push_back(entry[0]);
                table.push_back(added);
            }
        } else if (previous >= 0 && code == table.size()) {
            entry = table[static_cast<std::size_t>(previous)];
            entry.push_back(entry[0]);
            table.push_back(entry);
        } else {
            fail("TIFF", "LZW符号が不正です");
        }
        out.insert(out.end(), entry.begin(), entry.end());
        if (out.size() >= expected) break;
        previous = static_cast<int>(code);
        if (table.size() + 1 >= (1u << width) && width < 12) ++width;
    }
    return out;
}

std::vector<std::uint8_t> packbits_decode(const std::uint8_t* data, std::size_t size,
                                          std::size_t expected) {
    std::vector<std::uint8_t> out;
    out.reserve(expected);
    std::size_t i = 0;
    while (i < size && out.size() < expected) {
        const int n = static_cast<std::int8_t>(data[i++]);
        if (n >= 0) {
            const std::size_t count = static_cast<std::size_t>(n) + 1;
            if (i + count > size) fail("TIFF", "PackBitsのデータが切れています");
            out.insert(out.end(), data + i, data + i + count);
            i += count;
        } else if (n != -128) {
            if (i >= size) fail("TIFF", "PackBitsのデータが切れています");
            out.insert(out.end(), static_cast<std::size_t>(1 - n), data[i++]);
        }
    }
    return out;
}

void read_tiff(const std::vector<std::uint8_t>& data, FrameBuffer& out, ImageFileInfo& info,
               bool header_only) {
    if (data.size() < 8) fail("TIFF", "ファイルが短すぎます");
    TiffReader r(data);
    if (data[0] == 'I' && data[1] == 'I') r.little = true;
    else if (data[0] == 'M' && data[1] == 'M') r.little = false;
    else fail("TIFF", "バイトオーダーの印がありません");
    const std::uint16_t magic = r.u16(2);
    if (magic == 43) fail("TIFF", "BigTIFFには対応していません");
    if (magic != 42) fail("TIFF", "TIFFの識別子がありません");

    const std::size_t ifd = r.u32(4);
    const std::uint16_t entries = r.u16(ifd);
    std::vector<std::pair<std::uint16_t, TiffTag>> tags;
    for (std::uint16_t i = 0; i < entries; ++i) {
        const std::size_t e = ifd + 2 + static_cast<std::size_t>(i) * 12;
        TiffTag t;
        const std::uint16_t tag = r.u16(e);
        t.type = r.u16(e + 2);
        t.count = r.u32(e + 4);
        const std::size_t bytes = static_cast<std::size_t>(tiff_type_size(t.type)) * t.count;
        t.value_offset = bytes <= 4 ? e + 8 : r.u32(e + 8);
        tags.push_back(std::make_pair(tag, t));
    }
    const auto find = [&](std::uint16_t tag) -> const TiffTag* {
        for (const auto& p : tags) if (p.first == tag) return &p.second;
        return nullptr;
    };
    const auto scalar = [&](std::uint16_t tag, std::uint32_t fallback) -> std::uint32_t {
        const TiffTag* t = find(tag);
        if (!t || t->count == 0) return fallback;
        return tiff_values(r, *t)[0];
    };

    const int w = static_cast<int>(scalar(256, 0));
    const int h = static_cast<int>(scalar(257, 0));
    const int spp = static_cast<int>(scalar(277, 1));
    // 値の個数が0の欄は無いものとして扱う（空の並びの先頭を読まない）。
    const int bits = static_cast<int>(scalar(258, 1));
    const std::uint32_t compression = scalar(259, 1);
    const std::uint32_t photometric = scalar(262, 1);
    const std::uint32_t planar = scalar(284, 1);
    const std::uint32_t predictor = scalar(317, 1);
    const std::uint32_t sample_format = scalar(339, 1);

    check_image_size("TIFF", static_cast<std::uint32_t>(scalar(256, 0)), static_cast<std::uint32_t>(scalar(257, 0)));
    if (spp < 1 || spp > 16) fail("TIFF", "1画素あたりの標本数が不正です");
    if (bits != 8 && bits != 16 && bits != 32) {
        fail("TIFF", "対応していないビット深度です (" + std::to_string(bits) + "bit)");
    }
    if (photometric == 3) fail("TIFF", "パレット形式のTIFFには対応していません");
    if (sample_format == 3 && bits != 32) fail("TIFF", "32bit以外の浮動小数点には対応していません");
    const int color_samples = (photometric == 2) ? 3 : 1;
    if (spp < color_samples) fail("TIFF", "チャンネル数が色形式と合いません");

    info.width = w;
    info.height = h;
    info.channels = color_samples;
    info.bit_depth = bits;
    info.color = color_samples == 3 ? SerColorId::RGB : SerColorId::Mono;
    info.format = "TIFF";
    if (header_only) return;

    const bool tiled = find(322) != nullptr;
    const int chunk_w = tiled ? static_cast<int>(scalar(322, 0)) : w;
    const int chunk_h = tiled ? static_cast<int>(scalar(323, 0)) : static_cast<int>(std::min<std::uint32_t>(scalar(278, static_cast<std::uint32_t>(h)), static_cast<std::uint32_t>(h)));
    if (chunk_w <= 0 || chunk_h <= 0 || chunk_w > std::max(w, 4096) || chunk_h > std::max(h, 4096)) {
        fail("TIFF", "タイルまたはストリップの大きさが不正です");
    }
    const TiffTag* offsets_tag = find(tiled ? 324 : 273);
    const TiffTag* counts_tag = find(tiled ? 325 : 279);
    if (!offsets_tag || !counts_tag) fail("TIFF", "画素データの位置がありません");
    const std::vector<std::uint32_t> offsets = tiff_values(r, *offsets_tag);
    const std::vector<std::uint32_t> counts = tiff_values(r, *counts_tag);
    if (offsets.size() != counts.size()) fail("TIFF", "ストリップ情報が食い違っています");

    const int bytes_per_sample = bits / 8;
    const int samples_in_chunk = planar == 2 ? 1 : spp;
    const int across = (w + chunk_w - 1) / chunk_w;
    const int down = (h + chunk_h - 1) / chunk_h;
    const int planes = planar == 2 ? spp : 1;
    if (offsets.size() < static_cast<std::size_t>(across) * static_cast<std::size_t>(down) * static_cast<std::size_t>(planes)) {
        fail("TIFF", "ストリップの数が足りません");
    }

    out.reset(w, h, color_samples);
    out.set_source_bit_depth(bits);
    const std::size_t row_bytes =
        static_cast<std::size_t>(chunk_w) * samples_in_chunk * bytes_per_sample;
    const std::size_t chunk_bytes = row_bytes * static_cast<std::size_t>(chunk_h);

    for (int plane = 0; plane < planes; ++plane) {
        for (int ty = 0; ty < down; ++ty) {
            for (int tx = 0; tx < across; ++tx) {
                const std::size_t index =
                    static_cast<std::size_t>(plane) * across * down +
                    static_cast<std::size_t>(ty) * across + tx;
                r.need(offsets[index], counts[index]);
                const std::uint8_t* src = data.data() + offsets[index];
                const std::size_t src_size = counts[index];
                std::vector<std::uint8_t> raw;
                if (compression == 1) {
                    raw.assign(src, src + std::min(src_size, chunk_bytes));
                } else if (compression == 5) {
                    raw = tiff_lzw_decode(src, src_size, chunk_bytes);
                } else if (compression == 8 || compression == 32946) {
                    raw = inflate_zlib(src, src_size, chunk_bytes);
                } else if (compression == 32773) {
                    raw = packbits_decode(src, src_size, chunk_bytes);
                } else {
                    fail("TIFF", "対応していない圧縮形式です (" + std::to_string(compression) + ")");
                }
                // ストリップの最後は画像の下端で切れていてよい。
                const int rows_here = std::min(chunk_h, h - ty * chunk_h);
                const std::size_t needed = row_bytes * static_cast<std::size_t>(tiled ? chunk_h : rows_here);
                if (raw.size() < needed) raw.resize(needed, 0);

                // 予測子を戻す。
                if (predictor == 2) {
                    for (int y = 0; y < rows_here; ++y) {
                        std::uint8_t* row = raw.data() + static_cast<std::size_t>(y) * row_bytes;
                        for (int x = 1; x < chunk_w; ++x) {
                            for (int s = 0; s < samples_in_chunk; ++s) {
                                const std::size_t cur = (static_cast<std::size_t>(x) * samples_in_chunk + s) * bytes_per_sample;
                                const std::size_t prev = cur - static_cast<std::size_t>(samples_in_chunk) * bytes_per_sample;
                                if (bytes_per_sample == 1) {
                                    row[cur] = static_cast<std::uint8_t>(row[cur] + row[prev]);
                                } else if (bytes_per_sample == 2) {
                                    const auto get = [&](std::size_t o) {
                                        return r.little ? static_cast<unsigned>(row[o] | (row[o + 1] << 8))
                                                        : static_cast<unsigned>((row[o] << 8) | row[o + 1]);
                                    };
                                    const unsigned v = (get(cur) + get(prev)) & 0xFFFFu;
                                    if (r.little) { row[cur] = v & 0xFF; row[cur + 1] = (v >> 8) & 0xFF; }
                                    else { row[cur] = (v >> 8) & 0xFF; row[cur + 1] = v & 0xFF; }
                                } else {
                                    fail("TIFF", "32bit整数の水平差分予測には対応していません");
                                }
                            }
                        }
                    }
                } else if (predictor == 3) {
                    // 浮動小数点予測子: バイト単位の差分を戻してから、上位バイト順に
                    // 分けて並べられたバイト列を画素ごとに組み直す。
                    const std::size_t stride = static_cast<std::size_t>(samples_in_chunk);
                    std::vector<std::uint8_t> tmp(row_bytes);
                    for (int y = 0; y < rows_here; ++y) {
                        std::uint8_t* row = raw.data() + static_cast<std::size_t>(y) * row_bytes;
                        for (std::size_t i = stride; i < row_bytes; ++i) {
                            row[i] = static_cast<std::uint8_t>(row[i] + row[i - stride]);
                        }
                        const std::size_t count = static_cast<std::size_t>(chunk_w) * samples_in_chunk;
                        for (std::size_t i = 0; i < count; ++i) {
                            for (int b = 0; b < bytes_per_sample; ++b) {
                                // 格納は上位バイトから。TIFFのバイトオーダーに合わせて置き直す。
                                const std::uint8_t byte = row[static_cast<std::size_t>(b) * count + i];
                                const int dst = r.little ? (bytes_per_sample - 1 - b) : b;
                                tmp[i * bytes_per_sample + static_cast<std::size_t>(dst)] = byte;
                            }
                        }
                        std::memcpy(row, tmp.data(), row_bytes);
                    }
                }

                // 画素を取り出して配置する。
                for (int y = 0; y < rows_here; ++y) {
                    const int iy = ty * chunk_h + y;
                    const std::uint8_t* row = raw.data() + static_cast<std::size_t>(y) * row_bytes;
                    for (int x = 0; x < chunk_w; ++x) {
                        const int ix = tx * chunk_w + x;
                        if (ix >= w) break;
                        for (int s = 0; s < samples_in_chunk; ++s) {
                            const int channel = planar == 2 ? plane : s;
                            if (channel >= color_samples) continue;  // アルファ等
                            const std::size_t o = (static_cast<std::size_t>(x) * samples_in_chunk + s) * bytes_per_sample;
                            double v = 0.0;
                            if (bits == 8) {
                                v = row[o] / 255.0;
                            } else if (bits == 16) {
                                const unsigned u = r.little ? (row[o] | (row[o + 1] << 8))
                                                            : ((row[o] << 8) | row[o + 1]);
                                v = sample_format == 2 ? (static_cast<std::int16_t>(u) + 32768.0) / 65535.0
                                                       : u / 65535.0;
                            } else {
                                std::uint32_t u = r.little
                                    ? (static_cast<std::uint32_t>(row[o]) | (static_cast<std::uint32_t>(row[o + 1]) << 8) |
                                       (static_cast<std::uint32_t>(row[o + 2]) << 16) | (static_cast<std::uint32_t>(row[o + 3]) << 24))
                                    : ((static_cast<std::uint32_t>(row[o]) << 24) | (static_cast<std::uint32_t>(row[o + 1]) << 16) |
                                       (static_cast<std::uint32_t>(row[o + 2]) << 8) | row[o + 3]);
                                if (sample_format == 3) {
                                    float f;
                                    std::memcpy(&f, &u, sizeof(f));
                                    v = f;
                                } else if (sample_format == 2) {
                                    v = (static_cast<std::int32_t>(u) + 2147483648.0) / 4294967295.0;
                                } else {
                                    v = u / 4294967295.0;
                                }
                            }
                            if (photometric == 0) v = 1.0 - v;  // WhiteIsZero
                            out.row(channel, iy)[ix] = clamp01(v);
                        }
                    }
                }
            }
        }
    }
    if (sample_format == 3) normalize_float_scale(out);
    out.invalidate_luma();
}

// ---- PNG -------------------------------------------------------------------

std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

void read_png(const std::vector<std::uint8_t>& data, FrameBuffer& out, ImageFileInfo& info,
              bool header_only) {
    static const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (data.size() < 8 + 25 || std::memcmp(data.data(), sig, 8) != 0) fail("PNG", "PNGの識別子がありません");
    std::size_t pos = 8;
    int w = 0, h = 0, depth = 0, color = 0, interlace = 0;
    std::vector<std::uint8_t> idat;
    std::vector<std::uint8_t> palette;
    bool seen_ihdr = false;
    while (pos + 12 <= data.size()) {
        const std::uint32_t len = be32(&data[pos]);
        const std::string type(reinterpret_cast<const char*>(&data[pos + 4]), 4);
        if (pos + 12 + len > data.size()) fail("PNG", "チャンクが途中で切れています");
        const std::uint8_t* body = &data[pos + 8];
        if (type == "IHDR") {
            if (len < 13) fail("PNG", "IHDRが短すぎます");
            w = static_cast<int>(be32(body));
            h = static_cast<int>(be32(body + 4));
            depth = body[8];
            color = body[9];
            interlace = body[12];
            seen_ihdr = true;
            if (header_only) break;
        } else if (type == "PLTE") {
            palette.assign(body, body + len);
        } else if (type == "IDAT") {
            idat.insert(idat.end(), body, body + len);
        } else if (type == "IEND") {
            break;
        }
        pos += 12 + len;
    }
    if (!seen_ihdr) fail("PNG", "IHDRがありません");
    check_image_size("PNG", static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
    const int channels_in = color == 0 ? 1 : color == 2 ? 3 : color == 3 ? 1 : color == 4 ? 2 : color == 6 ? 4 : 0;
    if (channels_in == 0) fail("PNG", "不明な色形式です");
    const int color_out = (color == 2 || color == 3 || color == 6) ? 3 : 1;
    info.width = w;
    info.height = h;
    info.channels = color_out;
    info.bit_depth = color == 3 ? 8 : depth;
    info.color = color_out == 3 ? SerColorId::RGB : SerColorId::Mono;
    info.format = "PNG";
    if (header_only) return;
    if (interlace != 0) fail("PNG", "インターレースPNGには対応していません");
    if (color == 3 && palette.empty()) fail("PNG", "パレットがありません");

    const std::size_t bits_per_pixel = static_cast<std::size_t>(channels_in) * depth;
    const std::size_t row_bytes = (static_cast<std::size_t>(w) * bits_per_pixel + 7) / 8;
    const std::size_t bpp = std::max<std::size_t>(1, bits_per_pixel / 8);
    const std::vector<std::uint8_t> raw =
        inflate_zlib(idat.data(), idat.size(), (row_bytes + 1) * static_cast<std::size_t>(h));
    if (raw.size() < (row_bytes + 1) * static_cast<std::size_t>(h)) fail("PNG", "画素データが足りません");

    out.reset(w, h, color_out);
    out.set_source_bit_depth(info.bit_depth);
    std::vector<std::uint8_t> prev(row_bytes, 0), cur(row_bytes, 0);
    const double max_value = static_cast<double>((1u << depth) - 1u);
    // 画素値は倍精度で割ってから丸める（逆数の掛け算は正しく丸まらない）。
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* src = &raw[static_cast<std::size_t>(y) * (row_bytes + 1)];
        const int filter = src[0];
        const std::uint8_t* x = src + 1;
        switch (filter) {
            case 0:
                std::memcpy(cur.data(), x, row_bytes);
                break;
            case 1:
                for (std::size_t i = 0; i < row_bytes; ++i) {
                    cur[i] = static_cast<std::uint8_t>(x[i] + (i >= bpp ? cur[i - bpp] : 0));
                }
                break;
            case 2:
                for (std::size_t i = 0; i < row_bytes; ++i) cur[i] = static_cast<std::uint8_t>(x[i] + prev[i]);
                break;
            case 3:
                for (std::size_t i = 0; i < row_bytes; ++i) {
                    const int a = i >= bpp ? cur[i - bpp] : 0;
                    cur[i] = static_cast<std::uint8_t>(x[i] + ((a + prev[i]) >> 1));
                }
                break;
            case 4:
                for (std::size_t i = 0; i < row_bytes; ++i) {
                    const int a = i >= bpp ? cur[i - bpp] : 0;
                    const int c = i >= bpp ? prev[i - bpp] : 0;
                    cur[i] = static_cast<std::uint8_t>(x[i] + paeth(a, prev[i], c));
                }
                break;
            default:
                fail("PNG", "不明なフィルタです");
        }
        if (color != 3 && depth == 16) {
            for (int c = 0; c < color_out; ++c) {
                float* dst = out.row(c, y);
                for (int px = 0; px < w; ++px) {
                    const std::size_t o = (static_cast<std::size_t>(px) * channels_in + c) * 2;
                    dst[px] = static_cast<float>(((cur[o] << 8) | cur[o + 1]) / 65535.0);
                }
            }
        } else if (color != 3 && depth == 8) {
            for (int c = 0; c < color_out; ++c) {
                float* dst = out.row(c, y);
                for (int px = 0; px < w; ++px) {
                    dst[px] = static_cast<float>(cur[static_cast<std::size_t>(px) * channels_in + c] / 255.0);
                }
            }
        } else {
            const auto sample = [&](int index) -> unsigned {
                if (depth == 8) return cur[static_cast<std::size_t>(index)];
                const std::size_t bit = static_cast<std::size_t>(index) * depth;
                return (cur[bit / 8] >> (8 - depth - (bit % 8))) & ((1u << depth) - 1u);
            };
            for (int px = 0; px < w; ++px) {
                if (color == 3) {
                    const unsigned p = sample(px);
                    if (p * 3 + 2 >= palette.size()) fail("PNG", "パレット番号が範囲外です");
                    for (int c = 0; c < 3; ++c) out.row(c, y)[px] = static_cast<float>(palette[p * 3 + c] / 255.0);
                } else {
                    for (int c = 0; c < color_out; ++c) {
                        out.row(c, y)[px] = static_cast<float>(sample(px * channels_in + c) / max_value);
                    }
                }
            }
        }
        std::swap(prev, cur);
    }
    out.invalidate_luma();
}

// ---- FITS ------------------------------------------------------------------

std::string fits_trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\'')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\'')) --b;
    return s.substr(a, b - a);
}

void read_fits(const std::vector<std::uint8_t>& data, FrameBuffer& out, ImageFileInfo& info,
               bool header_only) {
    if (data.size() < 2880 || std::memcmp(data.data(), "SIMPLE  =", 9) != 0) {
        fail("FITS", "FITSの識別子がありません");
    }
    int bitpix = 0, naxis = 0, n1 = 0, n2 = 0, n3 = 1;
    double bzero = 0.0, bscale = 1.0;
    std::string bayer, roworder;
    std::size_t pos = 0;
    bool ended = false;
    while (!ended) {
        if (pos + 2880 > data.size()) fail("FITS", "ヘッダが途中で切れています");
        for (int card = 0; card < 36; ++card) {
            const std::string line(reinterpret_cast<const char*>(&data[pos + card * 80]), 80);
            const std::string key = fits_trim(line.substr(0, 8));
            if (key == "END") { ended = true; break; }
            if (line.size() < 10 || line[8] != '=') continue;
            std::string value = line.substr(10);
            // 文字列値は引用符の中、それ以外は '/' の前までが値。
            const std::size_t first = value.find_first_not_of(' ');
            if (first != std::string::npos && value[first] == '\'') {
                const std::size_t q1 = value.find('\'');
                const std::size_t q2 = value.find('\'', q1 + 1);
                value = q2 == std::string::npos ? value.substr(q1 + 1) : value.substr(q1 + 1, q2 - q1 - 1);
            } else {
                const std::size_t slash = value.find('/');
                if (slash != std::string::npos) value = value.substr(0, slash);
            }
            value = fits_trim(value);
            if (key == "BITPIX") bitpix = std::atoi(value.c_str());
            else if (key == "NAXIS") naxis = std::atoi(value.c_str());
            else if (key == "NAXIS1") n1 = std::atoi(value.c_str());
            else if (key == "NAXIS2") n2 = std::atoi(value.c_str());
            else if (key == "NAXIS3") n3 = std::atoi(value.c_str());
            else if (key == "BZERO") bzero = std::atof(value.c_str());
            else if (key == "BSCALE") bscale = std::atof(value.c_str());
            else if (key == "BAYERPAT" || key == "COLORTYP") bayer = value;
            else if (key == "ROWORDER") roworder = value;
        }
        pos += 2880;
    }
    if (naxis < 2 || naxis > 3 || n1 <= 0 || n2 <= 0) fail("FITS", "2軸または3軸の画像ではありません");
    if (naxis == 2) n3 = 1;
    if (n3 != 1 && n3 != 3) fail("FITS", "3軸目は1または3面のみ対応しています");
    if (bitpix != 8 && bitpix != 16 && bitpix != 32 && bitpix != -32 && bitpix != -64) {
        fail("FITS", "対応していないBITPIXです (" + std::to_string(bitpix) + ")");
    }

    SerColorId color = n3 == 3 ? SerColorId::RGB : SerColorId::Mono;
    if (n3 == 1) {
        if (bayer == "RGGB") color = SerColorId::BayerRGGB;
        else if (bayer == "GRBG") color = SerColorId::BayerGRBG;
        else if (bayer == "GBRG") color = SerColorId::BayerGBRG;
        else if (bayer == "BGGR") color = SerColorId::BayerBGGR;
    }
    info.width = n1;
    info.height = n2;
    info.channels = n3;
    info.bit_depth = std::abs(bitpix);
    info.color = color;
    info.format = "FITS";
    if (header_only) return;

    const int bytes = std::abs(bitpix) / 8;
    const std::size_t plane = static_cast<std::size_t>(n1) * n2;
    if (pos + plane * n3 * bytes > data.size()) fail("FITS", "画素データが足りません");
    out.reset(n1, n2, n3);
    out.set_source_bit_depth(std::min(16, std::abs(bitpix)));
    // FITSは標準で下端の行から並ぶことが多いが、ROWORDERが無いファイルの向きは
    // 書いたソフト次第である。LunaStack自身の書き出し（上端から）と往復できるよう、
    // ROWORDER='BOTTOM-UP' のときだけ上下を返す。
    const bool bottom_up = roworder == "BOTTOM-UP";
    const bool is_float = bitpix < 0;
    for (int c = 0; c < n3; ++c) {
        for (int y = 0; y < n2; ++y) {
            const int dy = bottom_up ? n2 - 1 - y : y;
            float* row = out.row(c, dy);
            const std::uint8_t* src = &data[pos + (static_cast<std::size_t>(c) * plane + static_cast<std::size_t>(y) * n1) * bytes];
            for (int x = 0; x < n1; ++x) {
                const std::uint8_t* p = src + static_cast<std::size_t>(x) * bytes;
                double raw = 0.0;
                if (bitpix == 8) raw = p[0];
                else if (bitpix == 16) raw = static_cast<std::int16_t>((p[0] << 8) | p[1]);
                else if (bitpix == 32) raw = static_cast<std::int32_t>(be32(p));
                else if (bitpix == -32) {
                    const std::uint32_t u = be32(p);
                    float f;
                    std::memcpy(&f, &u, 4);
                    raw = f;
                } else {
                    const std::uint64_t u = (static_cast<std::uint64_t>(be32(p)) << 32) | be32(p + 4);
                    double d;
                    std::memcpy(&d, &u, 8);
                    raw = d;
                }
                double v = bzero + bscale * raw;
                if (!is_float) {
                    const double maximum = bitpix == 8 ? 255.0 : bitpix == 16 ? 65535.0 : 4294967295.0;
                    v /= maximum;
                }
                row[x] = clamp01(v);
            }
        }
    }
    if (is_float) normalize_float_scale(out);
    out.invalidate_luma();
}

void read_jpeg(const std::vector<std::uint8_t>& data, FrameBuffer& out, ImageFileInfo& info) {
    decode_baseline_jpeg(data.data(), data.size(), out);
    info.width = out.width();
    info.height = out.height();
    info.channels = out.channels();
    info.bit_depth = 8;
    info.color = out.channels() == 3 ? SerColorId::RGB : SerColorId::Mono;
    info.format = "JPEG";
}

void dispatch(const std::string& path, FrameBuffer* out, ImageFileInfo& info) {
    const std::string ext = lower_extension(path);
    // カメラのRAWは大きい（2000万画素級で数十MB）ので、全体を読み込まずに
    // メモリへ割り付けて必要な部分だけ触る。ヘッダだけなら画素は展開しない。
    if (is_raw_image_path(path)) {
        if (out) read_raw_image(path, *out, info);
        else info = probe_raw_image(path);
        return;
    }
    const std::vector<std::uint8_t> data = read_all(path);
    FrameBuffer scratch;
    FrameBuffer& target = out ? *out : scratch;
    const bool header_only = out == nullptr;
    if (ext == "tif" || ext == "tiff") read_tiff(data, target, info, header_only);
    else if (ext == "png") read_png(data, target, info, header_only);
    else if (ext == "fit" || ext == "fits" || ext == "fts") read_fits(data, target, info, header_only);
    else if (ext == "jpg" || ext == "jpeg") read_jpeg(data, target, info);
    else throw std::runtime_error("対応していない画像形式です: " + path);
}

}  // namespace

bool is_supported_image_path(const std::string& path) {
    const std::string ext = lower_extension(path);
    return ext == "tif" || ext == "tiff" || ext == "png" || ext == "fit" || ext == "fits" ||
           ext == "fts" || ext == "jpg" || ext == "jpeg" || is_raw_image_path(path);
}

void read_image_file(const std::string& path, FrameBuffer& out, ImageFileInfo& info) {
    dispatch(path, &out, info);
}

ImageFileInfo probe_image_file(const std::string& path) {
    ImageFileInfo info;
    dispatch(path, nullptr, info);
    return info;
}

bool natural_less(const std::string& a, const std::string& b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool da = a[i] >= '0' && a[i] <= '9';
        const bool db = b[j] >= '0' && b[j] <= '9';
        if (da && db) {
            std::size_t ie = i, je = j;
            while (ie < a.size() && a[ie] >= '0' && a[ie] <= '9') ++ie;
            while (je < b.size() && b[je] >= '0' && b[je] <= '9') ++je;
            // 先頭の0を除いた桁数で比べ、同じなら文字列として比べる。
            std::size_t ia = i, jb = j;
            while (ia + 1 < ie && a[ia] == '0') ++ia;
            while (jb + 1 < je && b[jb] == '0') ++jb;
            if (ie - ia != je - jb) return ie - ia < je - jb;
            const int cmp = a.compare(ia, ie - ia, b, jb, je - jb);
            if (cmp != 0) return cmp < 0;
            i = ie;
            j = je;
        } else {
            if (a[i] != b[j]) return static_cast<unsigned char>(a[i]) < static_cast<unsigned char>(b[j]);
            ++i;
            ++j;
        }
    }
    if (a.size() - i != b.size() - j) return a.size() - i < b.size() - j;
    return a < b;  // 完全に同順位なら通常の順で決定論的に並べる
}

std::vector<std::string> list_image_sequence(const std::string& directory) {
    std::vector<std::string> names;
    DIR* dir = opendir(directory.c_str());
    if (!dir) throw std::runtime_error("フォルダを開けません: " + directory);
    while (struct dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.empty() || name[0] == '.') continue;
        if (!is_supported_image_path(name)) continue;
        const std::string full = directory + (directory.back() == '/' ? "" : "/") + name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        names.push_back(name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end(), natural_less);
    std::vector<std::string> paths;
    paths.reserve(names.size());
    for (const std::string& name : names) {
        paths.push_back(directory + (directory.back() == '/' ? "" : "/") + name);
    }
    return select_sequence_files(paths);
}

std::vector<std::string> select_sequence_files(const std::vector<std::string>& paths) {
    const auto kind = [](const std::string& path) -> std::string {
        const std::string ext = lower_extension(path);
        if (ext == "tiff") return "tif";
        if (ext == "jpeg") return "jpg";
        if (ext == "fits" || ext == "fts") return "fit";
        return ext;
    };
    std::vector<std::string> kinds;
    std::vector<int> counts;
    std::vector<bool> raw;
    for (const std::string& p : paths) {
        if (!is_supported_image_path(p)) continue;
        const std::string k = kind(p);
        const auto it = std::find(kinds.begin(), kinds.end(), k);
        if (it == kinds.end()) {
            kinds.push_back(k);
            counts.push_back(1);
            raw.push_back(is_raw_image_path(p));
        } else {
            ++counts[static_cast<std::size_t>(it - kinds.begin())];
        }
    }
    if (kinds.empty()) return std::vector<std::string>();
    const bool any_raw = std::find(raw.begin(), raw.end(), true) != raw.end();
    std::size_t best = kinds.size();
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        if (any_raw && !raw[i]) continue;
        if (best == kinds.size() || counts[i] > counts[best]) best = i;
    }
    std::vector<std::string> out;
    for (const std::string& p : paths) {
        if (is_supported_image_path(p) && kind(p) == kinds[best]) out.push_back(p);
    }
    return out;
}

}  // namespace stackcore
