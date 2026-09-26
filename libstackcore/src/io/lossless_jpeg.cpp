#include "stackcore/lossless_jpeg.hpp"

#include <stdexcept>
#include <string>

namespace stackcore {
namespace {

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("ロスレスJPEG: " + what);
}

// ハフマン表。9bit以下の符号は表引き、それより長い符号は符号長ごとの範囲で引く。
struct Huffman {
    static constexpr int kLookupBits = 9;
    bool defined = false;
    // 表引き: 上位 kLookupBits ビット → (符号長 << 8) | 値。0 は「表に無い（長い符号）」。
    std::uint16_t lookup[1 << kLookupBits] = {};
    // 長い符号: 符号長 L の最大符号（無ければ -1）と、その長さの先頭の値の位置。
    int maxcode[18] = {};
    int valptr[17] = {};
    int mincode[17] = {};
    std::uint8_t values[256] = {};

    void build(const std::uint8_t counts[16], const std::uint8_t* vals, int total) {
        for (int i = 0; i < total; ++i) values[i] = vals[i];
        for (int i = 0; i < (1 << kLookupBits); ++i) lookup[i] = 0;
        int code = 0, k = 0;
        for (int len = 1; len <= 16; ++len) {
            valptr[len] = k;
            mincode[len] = code;
            for (int i = 0; i < counts[len - 1]; ++i, ++k, ++code) {
                if (len <= kLookupBits) {
                    const int shift = kLookupBits - len;
                    for (int j = 0; j < (1 << shift); ++j) {
                        lookup[(code << shift) | j] = static_cast<std::uint16_t>((len << 8) | values[k]);
                    }
                }
            }
            maxcode[len] = counts[len - 1] ? code - 1 : -1;
            if (code > (1 << len)) fail("ハフマン表が不正です");
            code <<= 1;
        }
        maxcode[17] = 0x7fffffff;
        defined = true;
    }
};

// エントロピー符号化された部分を読むビット列。0xFF 00 の詰め物を外し、
// マーカー（0xFF の後に0以外）に当たったらそこで止まって0を供給する。
struct BitReader {
    const std::uint8_t* p = nullptr;
    const std::uint8_t* end = nullptr;
    std::uint64_t buffer = 0;
    int bits = 0;
    bool at_marker = false;

    void fill() {
        while (bits <= 56) {
            std::uint8_t b = 0;
            if (!at_marker && p < end) {
                b = *p;
                if (b == 0xFF) {
                    if (p + 1 < end && p[1] == 0x00) {
                        p += 2;
                    } else {
                        at_marker = true;
                        b = 0;
                    }
                } else {
                    ++p;
                }
            }
            buffer |= static_cast<std::uint64_t>(b) << (56 - bits);
            bits += 8;
        }
    }
    unsigned peek(int n) {
        if (bits < n) fill();
        return static_cast<unsigned>(buffer >> (64 - n));
    }
    void consume(int n) {
        buffer <<= n;
        bits -= n;
    }
    unsigned get(int n) {
        if (n == 0) return 0;
        const unsigned v = peek(n);
        consume(n);
        return v;
    }
    // リスタートマーカー（FF D0〜D7）まで進み、ビット列を空にする。
    void restart() {
        buffer = 0;
        bits = 0;
        at_marker = false;
        while (p + 1 < end) {
            if (p[0] == 0xFF && p[1] >= 0xD0 && p[1] <= 0xD7) {
                p += 2;
                return;
            }
            ++p;
        }
        fail("リスタートマーカーが見つかりません");
    }
};

int decode_symbol(BitReader& br, const Huffman& h) {
    const unsigned look = br.peek(Huffman::kLookupBits);
    const std::uint16_t entry = h.lookup[look];
    if (entry) {
        br.consume(entry >> 8);
        return entry & 0xFF;
    }
    // 長い符号。
    int code = static_cast<int>(br.get(Huffman::kLookupBits));
    int len = Huffman::kLookupBits;
    while (code > h.maxcode[len]) {
        code = (code << 1) | static_cast<int>(br.get(1));
        ++len;
        if (len > 16) fail("ハフマン符号が不正です");
    }
    return h.values[h.valptr[len] + code - h.mincode[len]];
}

struct Header {
    int precision = 0, height = 0, width = 0, components = 0;
    int component_id[4] = {};
    int table_of[4] = {};  // 成分ごとのハフマン表の番号
    int predictor = 1;
    int point_transform = 0;
    int restart_interval = 0;
    Huffman tables[4];
    const std::uint8_t* scan = nullptr;  // エントロピー符号化部の先頭
    bool sof = false;
};

Header parse_header(const std::uint8_t* data, std::size_t size, bool need_scan) {
    Header h;
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) fail("SOI がありません");
    std::size_t pos = 2;
    const auto u16 = [&](std::size_t o) -> unsigned {
        if (o + 2 > size) fail("ファイルが途中で切れています");
        return (static_cast<unsigned>(data[o]) << 8) | data[o + 1];
    };
    for (;;) {
        if (pos + 2 > size) fail("マーカーの途中で切れています");
        if (data[pos] != 0xFF) fail("マーカーが並んでいません");
        while (pos < size && data[pos] == 0xFF) ++pos;  // 詰め物の FF
        if (pos >= size) fail("マーカーの途中で切れています");
        const std::uint8_t marker = data[pos++];
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) continue;
        if (marker == 0xD9) fail("画像データがありません");
        const unsigned length = u16(pos);
        if (length < 2 || pos + length > size) fail("区切りの長さが不正です");
        const std::uint8_t* seg = data + pos + 2;
        const std::size_t seg_len = length - 2;
        if (marker == 0xC3) {
            if (seg_len < 6) fail("SOF3 が短すぎます");
            h.precision = seg[0];
            h.height = (seg[1] << 8) | seg[2];
            h.width = (seg[3] << 8) | seg[4];
            h.components = seg[5];
            if (h.components < 1 || h.components > 4) fail("成分数に対応していません");
            if (seg_len < 6 + static_cast<std::size_t>(h.components) * 3) fail("SOF3 が短すぎます");
            for (int c = 0; c < h.components; ++c) {
                h.component_id[c] = seg[6 + c * 3];
                if (seg[7 + c * 3] != 0x11) {
                    fail("成分の標本化係数が1×1ではありません（sRAW/mRAW には対応していません）");
                }
            }
            if (h.precision < 2 || h.precision > 16) fail("精度に対応していません");
            if (h.width <= 0 || h.height <= 0) fail("画像の寸法がありません");
            h.sof = true;
            if (!need_scan) return h;
        } else if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
                   marker != 0xCC) {
            fail("ロスレス（SOF3）ではないJPEGです");
        } else if (marker == 0xC4) {
            std::size_t o = 0;
            while (o < seg_len) {
                if (o + 17 > seg_len) fail("DHT が短すぎます");
                const int tc = seg[o] >> 4, th = seg[o] & 15;
                if (tc != 0 || th > 3) fail("DHT の表番号が不正です");
                int total = 0;
                for (int i = 0; i < 16; ++i) total += seg[o + 1 + i];
                if (total > 256 || o + 17 + static_cast<std::size_t>(total) > seg_len) fail("DHT が短すぎます");
                h.tables[th].build(seg + o + 1, seg + o + 17, total);
                o += 17 + static_cast<std::size_t>(total);
            }
        } else if (marker == 0xDD) {
            if (seg_len < 2) fail("DRI が短すぎます");
            h.restart_interval = (seg[0] << 8) | seg[1];
        } else if (marker == 0xDA) {
            if (!h.sof) fail("SOF3 より前に SOS があります");
            if (seg_len < 1) fail("SOS が短すぎます");
            const int ns = seg[0];
            if (ns != h.components) fail("走査の成分数がフレームと違います（非インターリーブには対応していません）");
            if (seg_len < 1 + static_cast<std::size_t>(ns) * 2 + 3) fail("SOS が短すぎます");
            for (int i = 0; i < ns; ++i) {
                const int id = seg[1 + i * 2];
                int index = -1;
                for (int c = 0; c < h.components; ++c) {
                    if (h.component_id[c] == id) index = c;
                }
                if (index < 0) fail("走査の成分がフレームにありません");
                h.table_of[index] = seg[2 + i * 2] >> 4;
                if (h.table_of[index] > 3 || !h.tables[h.table_of[index]].defined) {
                    fail("ハフマン表が定義されていません");
                }
            }
            h.predictor = seg[1 + ns * 2];
            h.point_transform = seg[3 + ns * 2] & 15;
            if (h.predictor < 1 || h.predictor > 7) fail("予測子に対応していません");
            h.scan = seg + seg_len;
            return h;
        }
        pos += length;
    }
}

}  // namespace

LosslessJpegImage probe_lossless_jpeg(const std::uint8_t* data, std::size_t size) {
    const Header h = parse_header(data, size, false);
    if (!h.sof) fail("SOF3 がありません");
    LosslessJpegImage out;
    out.width = h.width;
    out.height = h.height;
    out.components = h.components;
    out.precision = h.precision;
    return out;
}

LosslessJpegImage decode_lossless_jpeg(const std::uint8_t* data, std::size_t size,
                                       std::size_t max_samples) {
    const Header h = parse_header(data, size, true);
    const int nc = h.components;
    const std::size_t row_samples = static_cast<std::size_t>(h.width) * nc;
    const std::size_t total = row_samples * static_cast<std::size_t>(h.height);
    if (max_samples && total > max_samples) fail("画像の寸法がデータの大きさと合いません");
    if (h.restart_interval && h.restart_interval % h.width != 0) {
        fail("行の途中でのリスタートには対応していません");
    }
    const int rows_per_restart = h.restart_interval ? h.restart_interval / h.width : 0;

    LosslessJpegImage out;
    out.width = h.width;
    out.height = h.height;
    out.components = nc;
    out.precision = h.precision;
    out.samples.assign(total, 0);

    BitReader br;
    br.p = h.scan;
    br.end = data + size;
    const int pt = h.point_transform;
    const int initial = 1 << (h.precision - pt - 1);
    const Huffman* table[4];
    for (int c = 0; c < nc; ++c) table[c] = &h.tables[h.table_of[c]];

    // 予測は点変換前（Pt ぶん右へずらした）の値で行う。出力は最後に左へ戻す。
    std::vector<std::uint16_t> previous(row_samples, 0), current(row_samples, 0);
    bool first_line = true;
    for (int y = 0; y < h.height; ++y) {
        if (rows_per_restart && y > 0 && y % rows_per_restart == 0) {
            br.restart();
            first_line = true;  // リスタート区間の先頭の行は、走査の先頭と同じ扱い
        }
        for (int x = 0; x < h.width; ++x) {
            for (int c = 0; c < nc; ++c) {
                const std::size_t i = static_cast<std::size_t>(x) * nc + c;
                int pred;
                if (first_line) {
                    pred = x == 0 ? initial : current[i - nc];
                } else if (x == 0) {
                    pred = previous[i];
                } else {
                    const int ra = current[i - nc], rb = previous[i], rc = previous[i - nc];
                    switch (h.predictor) {
                        case 1: pred = ra; break;
                        case 2: pred = rb; break;
                        case 3: pred = rc; break;
                        case 4: pred = ra + rb - rc; break;
                        case 5: pred = ra + ((rb - rc) >> 1); break;
                        case 6: pred = rb + ((ra - rc) >> 1); break;
                        default: pred = (ra + rb) >> 1; break;
                    }
                }
                const int s = decode_symbol(br, *table[c]);
                int diff;
                if (s == 0) {
                    diff = 0;
                } else if (s == 16) {
                    diff = 32768;
                } else if (s > 16) {
                    fail("差分の桁数が不正です");
                } else {
                    diff = static_cast<int>(br.get(s));
                    if (diff < (1 << (s - 1))) diff -= (1 << s) - 1;
                }
                current[i] = static_cast<std::uint16_t>((pred + diff) & 0xFFFF);
            }
        }
        std::uint16_t* dst = out.samples.data() + static_cast<std::size_t>(y) * row_samples;
        for (std::size_t i = 0; i < row_samples; ++i) {
            dst[i] = static_cast<std::uint16_t>(current[i] << pt);
        }
        previous.swap(current);
        first_line = false;
    }
    return out;
}

}  // namespace stackcore
