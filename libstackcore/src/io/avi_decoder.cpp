#include "stackcore/avi_decoder.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "stackcore/jpeg_decoder.hpp"

namespace stackcore {
namespace {

constexpr std::uint64_t kChunkHeaderBytes = 8;

std::uint32_t read_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::int32_t read_i32(const std::uint8_t* p) {
    return static_cast<std::int32_t>(read_u32(p));
}

std::uint16_t read_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                      (static_cast<std::uint16_t>(p[1]) << 8));
}

bool fourcc_is(const std::uint8_t* p, const char* s) {
    return p[0] == static_cast<std::uint8_t>(s[0]) && p[1] == static_cast<std::uint8_t>(s[1]) &&
           p[2] == static_cast<std::uint8_t>(s[2]) && p[3] == static_cast<std::uint8_t>(s[3]);
}

std::string fourcc_string(const std::uint8_t* p) {
    std::string s;
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>(p[i]);
        s.push_back(c >= 32 && c < 127 ? c : '.');
    }
    return s;
}

std::string compression_name_of(std::uint32_t compression) {
    if (compression == 0) return "BI_RGB (非圧縮)";
    std::uint8_t b[4] = {static_cast<std::uint8_t>(compression & 0xFF),
                         static_cast<std::uint8_t>((compression >> 8) & 0xFF),
                         static_cast<std::uint8_t>((compression >> 16) & 0xFF),
                         static_cast<std::uint8_t>((compression >> 24) & 0xFF)};
    return fourcc_string(b);
}

// 生画素として扱えるFourCCか。
// ここに無いものは圧縮形式とみなして拒否する（黙って誤ったデコードをしない）。
bool is_raw_fourcc(std::uint32_t c, int& out_bits, bool& out_is_gray) {
    struct Entry {
        const char* cc;
        int bits;
        bool gray;
    };
    static const Entry kTable[] = {
        {"Y800", 8, true},  {"GREY", 8, true},  {"Y8  ", 8, true},   {"y800", 8, true},
        {"Y16 ", 16, true}, {"b16g", 16, true}, {"RGB ", 24, false}, {"DIB ", 24, false},
    };
    const std::uint8_t b[4] = {static_cast<std::uint8_t>(c & 0xFF),
                               static_cast<std::uint8_t>((c >> 8) & 0xFF),
                               static_cast<std::uint8_t>((c >> 16) & 0xFF),
                               static_cast<std::uint8_t>((c >> 24) & 0xFF)};
    for (const Entry& e : kTable) {
        if (fourcc_is(b, e.cc)) {
            out_bits = e.bits;
            out_is_gray = e.gray;
            return true;
        }
    }
    return false;
}

bool is_mjpeg_fourcc(std::uint32_t compression) {
    const std::uint8_t b[4] = {static_cast<std::uint8_t>(compression & 0xFF),
                               static_cast<std::uint8_t>((compression >> 8) & 0xFF),
                               static_cast<std::uint8_t>((compression >> 16) & 0xFF),
                               static_cast<std::uint8_t>((compression >> 24) & 0xFF)};
    return fourcc_is(b, "MJPG") || fourcc_is(b, "mjpg") || fourcc_is(b, "JPEG") ||
           fourcc_is(b, "jpeg") || fourcc_is(b, "dmb1");
}

}  // namespace

int AviDecoder::planes() const noexcept {
    return color_id_ == SerColorId::RGB || color_id_ == SerColorId::BGR ? 3 : 1;
}

int AviDecoder::bit_depth() const noexcept {
    // 1サンプル（1画素1チャンネル）あたりのビット数。
    // 24bit BGR は 3プレーンなので 8、Y16 は 1プレーンなので 16。
    // 32bit BGRX は 1画素4バイトだが実データは8bit×3である。
    if (planes() == 3) return 8;
    return header_.bit_count >= 16 ? 16 : 8;
}

int AviDecoder::bytes_per_sample() const noexcept { return bit_depth() > 8 ? 2 : 1; }

std::size_t AviDecoder::frame_bytes(int index) const {
    if (index < 0 || index >= frame_count()) {
        throw std::out_of_range("AVI: フレーム番号が範囲外です");
    }
    return frames_[static_cast<std::size_t>(index)].size;
}

void AviDecoder::open(const std::string& path) {
    file_.open(path);
    if (file_.size() < 12) {
        throw std::runtime_error("AVI: ファイルが小さすぎます");
    }
    parse(file_.data(), file_.size());

    if (frames_.empty()) {
        throw std::runtime_error("AVI: 映像フレームが1つも見つかりませんでした");
    }
    header_.frame_count = static_cast<int>(frames_.size());

    // MJPEGはフレームごとに圧縮サイズが異なり、行バイト数を持たない。
    if (mjpeg_) {
        FrameBuffer probe;
        const FrameRef& first = frames_[0];
        decode_baseline_jpeg(file_.data() + first.offset, first.size, probe);
        if (probe.width() != header_.width || probe.height() != header_.height) {
            throw std::runtime_error("AVI: MJPEGフレームの寸法がAVIヘッダと一致しません (" +
                                     std::to_string(probe.width()) + "x" +
                                     std::to_string(probe.height()) + " / " +
                                     std::to_string(header_.width) + "x" +
                                     std::to_string(header_.height) + ")");
        }
        color_id_ = probe.channels() == 1 ? SerColorId::Mono : SerColorId::RGB;
        header_.bit_count = probe.channels() * 8;
        header_.row_bytes = 0;
        return;
    }

    // 1行あたりのバイト数を、実際のフレームサイズから決める。
    // DIBの本来の規約はDWORD境界への切り上げだが、揃えずに書く実装もあるため、
    // 「規約どおりの値」と「詰めた値」の両方を候補にして実測と突き合わせる。
    const std::size_t bits = static_cast<std::size_t>(header_.bit_count);
    const std::size_t w = static_cast<std::size_t>(header_.width);
    const std::size_t h = static_cast<std::size_t>(header_.height);
    const std::size_t packed_row = (w * bits + 7) / 8;
    const std::size_t padded_row = ((w * bits + 31) / 32) * 4;
    const std::size_t actual = frames_[0].size;

    if (actual == padded_row * h) {
        header_.row_bytes = padded_row;
    } else if (actual == packed_row * h) {
        header_.row_bytes = packed_row;
    } else if (h > 0 && actual >= packed_row * h) {
        header_.row_bytes = actual / h;
    } else {
        throw std::runtime_error(
            "AVI: フレームのバイト数が寸法と合いません（" + std::to_string(actual) +
            " バイト、" + std::to_string(header_.width) + "x" + std::to_string(header_.height) +
            " " + std::to_string(header_.bit_count) + "bit には最低 " +
            std::to_string(packed_row * h) + " バイト必要）");
    }
}

void AviDecoder::parse(const std::uint8_t* data, std::uint64_t size) {
    if (!fourcc_is(data, "RIFF") || !fourcc_is(data + 8, "AVI ")) {
        throw std::runtime_error("AVI: RIFF/AVI ヘッダが見つかりません（AVIファイルではない可能性があります）");
    }

    bool have_strh = false, have_strf = false;
    int stream_index = 0;

    // ---- hdrl を読む -----------------------------------------------------
    // RIFF直下を走査し、LIST 'hdrl' → LIST 'strl' → strh/strf を拾う。
    // 最初に見つかった 'vids' ストリームを対象にする。
    std::uint64_t off = 12;
    const std::uint64_t riff_end = size;
    while (off + kChunkHeaderBytes <= riff_end) {
        const std::uint8_t* p = data + off;
        const std::uint32_t chunk_size = read_u32(p + 4);
        const std::uint64_t body = off + kChunkHeaderBytes;
        if (body + chunk_size > riff_end) break;

        if (fourcc_is(p, "LIST") && chunk_size >= 4 && fourcc_is(data + body, "hdrl")) {
            std::uint64_t h = body + 4;
            const std::uint64_t hend = body + chunk_size;
            while (h + kChunkHeaderBytes <= hend) {
                const std::uint8_t* hp = data + h;
                const std::uint32_t hsize = read_u32(hp + 4);
                const std::uint64_t hbody = h + kChunkHeaderBytes;
                if (hbody + hsize > hend) break;

                if (fourcc_is(hp, "LIST") && hsize >= 4 && fourcc_is(data + hbody, "strl")) {
                    bool this_is_video = false;
                    std::uint64_t s = hbody + 4;
                    const std::uint64_t send = hbody + hsize;
                    while (s + kChunkHeaderBytes <= send) {
                        const std::uint8_t* sp = data + s;
                        const std::uint32_t ssize = read_u32(sp + 4);
                        const std::uint64_t sbody = s + kChunkHeaderBytes;
                        if (sbody + ssize > send) break;

                        if (fourcc_is(sp, "strh") && ssize >= 32) {
                            const std::uint8_t* b = data + sbody;
                            this_is_video = fourcc_is(b, "vids");
                            if (this_is_video && !have_strh) {
                                header_.handler = fourcc_string(b + 4);
                                const std::uint32_t scale = read_u32(b + 20);
                                const std::uint32_t rate = read_u32(b + 24);
                                header_.fps = scale > 0 ? static_cast<double>(rate) / scale : 0.0;
                                video_stream_ = stream_index;
                                have_strh = true;
                            }
                        } else if (fourcc_is(sp, "strf") && this_is_video && !have_strf &&
                                   ssize >= 40) {
                            const std::uint8_t* b = data + sbody;
                            const std::int32_t bi_width = read_i32(b + 4);
                            const std::int32_t bi_height = read_i32(b + 8);
                            header_.bit_count = read_u16(b + 14);
                            header_.compression = read_u32(b + 16);
                            header_.width = bi_width;
                            // 上下の向き。
                            //
                            // 「下から上」はDIB（BI_RGB）固有の規約であり、
                            // FourCC形式は自分で並び順を定義するので常に上から下になる。
                            // ffmpegは Y800 を biHeight=+48（正）で書くが、
                            // 読むときは上から下として扱う。biHeightの符号だけを見て
                            // 分岐すると、FourCC形式で上下が反転する（実測で確認）。
                            //
                            // biHeightが負の場合は、形式によらず「上から下」を意味する。
                            header_.top_down = bi_height < 0 || header_.compression != 0;
                            header_.height = bi_height < 0 ? -bi_height : bi_height;
                            have_strf = true;
                        }
                        s = sbody + ssize + (ssize & 1);
                    }
                    if (this_is_video && have_strf) {
                        // 対象ストリームが確定した
                    }
                    ++stream_index;
                }
                h = hbody + hsize + (hsize & 1);
            }
        }
        off = body + chunk_size + (chunk_size & 1);
    }

    if (!have_strh || !have_strf) {
        throw std::runtime_error("AVI: 映像ストリームのヘッダ（strh/strf）が見つかりません");
    }
    if (header_.width <= 0 || header_.height <= 0 || header_.width > 65535 ||
        header_.height > 65535) {
        throw std::runtime_error("AVI: 画像サイズが不正です (" + std::to_string(header_.width) +
                                 "x" + std::to_string(header_.height) + ")");
    }

    header_.compression_name = compression_name_of(header_.compression);

    // ---- 対応する画素形式か判定する --------------------------------------
    int fourcc_bits = 0;
    bool fourcc_gray = false;
    if (header_.compression == 0) {
        // BI_RGB。ビット深度で色形式が決まる。
        if (header_.bit_count == 24 || header_.bit_count == 32) {
            color_id_ = SerColorId::BGR;  // DIBのRGBはメモリ上BGR順
        } else if (header_.bit_count == 8) {
            color_id_ = SerColorId::Mono;
        } else {
            throw std::runtime_error("AVI: 未対応のビット深度です (" +
                                     std::to_string(header_.bit_count) + " bit)");
        }
    } else if (is_raw_fourcc(header_.compression, fourcc_bits, fourcc_gray)) {
        color_id_ = fourcc_gray ? SerColorId::Mono : SerColorId::BGR;
        if (header_.bit_count == 0) header_.bit_count = fourcc_bits;
    } else if (is_mjpeg_fourcc(header_.compression)) {
        // JPEG側で成分順をRGBへ正規化するため、上流にはRGBとして渡す。
        mjpeg_ = true;
        color_id_ = SerColorId::RGB;
        header_.bit_count = 24;
    } else {
        // 未対応圧縮はここで明示的に拒否する。黙って生画素として読むと
        // ノイズのような画像を出したうえで「読めた」と主張してしまう。
        throw std::runtime_error("AVI: 未対応の圧縮形式です (" + header_.compression_name +
                                 ")。対応形式は非圧縮またはMJPEGです");
    }

    // ---- movi を走査してフレーム位置を集める -----------------------------
    // OpenDML(AVI 2.0)では2GBごとに RIFF 'AVIX' セグメントが続くため、
    // 先頭のRIFFだけでなくファイル末尾まで全セグメントを見る。
    // インデックス（idx1 / indx）は書き手による欠落や不整合があるため使わず、
    // movi の中身を直接数える。
    std::uint64_t seg = 0;
    while (seg + 12 <= size) {
        const std::uint8_t* p = data + seg;
        if (!fourcc_is(p, "RIFF")) break;
        const std::uint32_t riff_size = read_u32(p + 4);
        std::uint64_t seg_end = seg + kChunkHeaderBytes + riff_size;
        if (seg_end > size) seg_end = size;  // 切り詰められたファイルでも読めるだけ読む

        std::uint64_t c = seg + 12;
        while (c + kChunkHeaderBytes <= seg_end) {
            const std::uint8_t* cp = data + c;
            const std::uint32_t csize = read_u32(cp + 4);
            const std::uint64_t cbody = c + kChunkHeaderBytes;
            if (cbody + csize > seg_end) break;
            if (fourcc_is(cp, "LIST") && csize >= 4 && fourcc_is(data + cbody, "movi")) {
                scan_movi(data, cbody + 4, cbody + csize);
            }
            c = cbody + csize + (csize & 1);
        }
        seg = seg_end + (seg_end & 1);
    }
}

void AviDecoder::scan_movi(const std::uint8_t* data, std::uint64_t begin, std::uint64_t end) {
    const char digit0 = static_cast<char>('0' + (video_stream_ / 10) % 10);
    const char digit1 = static_cast<char>('0' + video_stream_ % 10);

    std::uint64_t off = begin;
    while (off + kChunkHeaderBytes <= end) {
        const std::uint8_t* p = data + off;
        const std::uint32_t csize = read_u32(p + 4);
        const std::uint64_t body = off + kChunkHeaderBytes;
        if (body + csize > end) break;

        if (fourcc_is(p, "LIST") && csize >= 4 && fourcc_is(data + body, "rec ")) {
            // フレームを 'rec ' でまとめる書き手がある。中身を見る。
            scan_movi(data, body + 4, body + csize);
        } else if (static_cast<char>(p[0]) == digit0 && static_cast<char>(p[1]) == digit1 &&
                   static_cast<char>(p[2]) == 'd' &&
                   (static_cast<char>(p[3]) == 'c' || static_cast<char>(p[3]) == 'b')) {
            // 'NNdc'（圧縮データ）と 'NNdb'（非圧縮データ）のどちらもフレーム。
            // ffmpegは非圧縮でも 'dc' を使うため、両方を受ける。
            FrameRef ref;
            ref.offset = body;
            ref.size = csize;
            frames_.push_back(ref);
        }
        off = body + csize + (csize & 1);
    }
}

void AviDecoder::read_frame(int index, FrameBuffer& out) const {
    if (index < 0 || index >= frame_count()) {
        throw std::out_of_range("AVI: フレーム番号が範囲外です (" + std::to_string(index) +
                                ", 有効範囲 0.." + std::to_string(frame_count() - 1) + ")");
    }
    const FrameRef& ref = frames_[static_cast<std::size_t>(index)];
    const int w = header_.width;
    const int h = header_.height;
    const int ch = planes();
    const std::size_t row_bytes = header_.row_bytes;

    if (mjpeg_) {
        decode_baseline_jpeg(file_.data() + ref.offset, ref.size, out);
        if (out.width() != w || out.height() != h) {
            throw std::runtime_error("AVI: MJPEGフレームの寸法がAVIヘッダと一致しません (" +
                                     std::to_string(out.width()) + "x" +
                                     std::to_string(out.height()) + " / " +
                                     std::to_string(w) + "x" + std::to_string(h) + ")");
        }
        if (out.channels() != 1 && out.channels() != 3) {
            throw std::runtime_error("AVI: MJPEGフレームの成分数が不正です");
        }
        file_.note_read(ref.size);
        return;
    }

    if (ref.size < row_bytes * static_cast<std::size_t>(h)) {
        throw std::runtime_error("AVI: フレーム " + std::to_string(index) +
                                 " のデータが不足しています");
    }

    if (out.width() != w || out.height() != h || out.channels() != ch) {
        out.reset(w, h, ch);
    }
    out.set_source_bit_depth(bit_depth());

    const std::uint8_t* base = file_.data() + ref.offset;
    const bool sixteen = bit_depth() == 16;
    const float inv = sixteen ? 1.0f / 65535.0f : 1.0f / 255.0f;

    for (int y = 0; y < h; ++y) {
        // 下から上に格納されている場合は、読み出す行を反転する。
        const int src_y = header_.top_down ? y : (h - 1 - y);
        const std::uint8_t* row = base + static_cast<std::size_t>(src_y) * row_bytes;

        if (ch == 1) {
            float* dst = out.row(0, y);
            if (sixteen) {
                for (int x = 0; x < w; ++x) {
                    const std::uint16_t v = static_cast<std::uint16_t>(
                        row[x * 2] | (static_cast<std::uint16_t>(row[x * 2 + 1]) << 8));
                    dst[x] = static_cast<float>(v) * inv;
                }
            } else {
                for (int x = 0; x < w; ++x) dst[x] = static_cast<float>(row[x]) * inv;
            }
        } else {
            // DIBの24/32bitはメモリ上 B, G, R (, X) の順に並ぶ。
            const int step = header_.bit_count / 8;
            float* r = out.row(0, y);
            float* g = out.row(1, y);
            float* b = out.row(2, y);
            for (int x = 0; x < w; ++x) {
                const std::uint8_t* px = row + static_cast<std::size_t>(x) * step;
                b[x] = static_cast<float>(px[0]) * inv;
                g[x] = static_cast<float>(px[1]) * inv;
                r[x] = static_cast<float>(px[2]) * inv;
            }
        }
    }
    out.invalidate_luma();
    file_.note_read(ref.size);
}

FrameStats AviDecoder::frame_stats(int index) const {
    if (index < 0 || index >= frame_count()) {
        throw std::out_of_range("AVI: フレーム番号が範囲外です");
    }
    const FrameRef& ref = frames_[static_cast<std::size_t>(index)];
    if (mjpeg_) {
        FrameBuffer decoded;
        read_frame(index, decoded);
        FrameStats stats;
        stats.min_value = 0xFFFFFFFFu;
        stats.max_value = 0;
        double sum = 0.0;
        std::size_t count = 0;
        for (int c = 0; c < decoded.channels(); ++c) {
            for (int y = 0; y < decoded.height(); ++y) {
                const float* row = decoded.row(c, y);
                for (int x = 0; x < decoded.width(); ++x) {
                    const std::uint32_t value = static_cast<std::uint32_t>(
                        std::max(0.0f, std::min(255.0f, row[x] * 255.0f + 0.5f)));
                    stats.min_value = std::min(stats.min_value, value);
                    stats.max_value = std::max(stats.max_value, value);
                    sum += value;
                    ++count;
                }
            }
        }
        if (count == 0) {
            stats.min_value = 0;
        } else {
            stats.mean_value = sum / static_cast<double>(count);
        }
        return stats;
    }
    const int w = header_.width;
    const int h = header_.height;
    const std::size_t row_bytes = header_.row_bytes;
    const bool sixteen = bit_depth() == 16;
    const int step = sixteen ? 2 : 1;
    const int samples_per_pixel = planes() == 3 ? header_.bit_count / 8 : 1;

    FrameStats stats;
    stats.min_value = 0xFFFFFFFFu;
    stats.max_value = 0;
    double sum = 0.0;
    std::size_t count = 0;

    const std::uint8_t* base = file_.data() + ref.offset;
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* row = base + static_cast<std::size_t>(y) * row_bytes;
        const int used = planes() == 3 ? 3 : 1;
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* px = row + static_cast<std::size_t>(x) * samples_per_pixel;
            for (int c = 0; c < used; ++c) {
                std::uint32_t v;
                if (sixteen) {
                    v = static_cast<std::uint32_t>(px[c * step]) |
                        (static_cast<std::uint32_t>(px[c * step + 1]) << 8);
                } else {
                    v = px[c];
                }
                if (v < stats.min_value) stats.min_value = v;
                if (v > stats.max_value) stats.max_value = v;
                sum += v;
                ++count;
            }
        }
    }
    if (count == 0) {
        stats.min_value = 0;
        return stats;
    }
    stats.mean_value = sum / static_cast<double>(count);
    return stats;
}

}  // namespace stackcore
