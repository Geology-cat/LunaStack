#include "stackcore/ser_decoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace stackcore {
namespace {

// SER v3 のヘッダは固定178バイト。ヘッダ内の整数は
// LittleEndian フラグの値に関係なく常にリトルエンディアンで格納される
// （あのフラグは16bit画素データのみに適用される）。
constexpr std::size_t kHeaderBytes = 178;

std::int32_t read_i32(const std::uint8_t* p) {
    const std::uint32_t v = static_cast<std::uint32_t>(p[0]) |
                            (static_cast<std::uint32_t>(p[1]) << 8) |
                            (static_cast<std::uint32_t>(p[2]) << 16) |
                            (static_cast<std::uint32_t>(p[3]) << 24);
    return static_cast<std::int32_t>(v);
}

std::int64_t read_i64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | static_cast<std::uint64_t>(p[i]);
    }
    return static_cast<std::int64_t>(v);
}

// 固定長フィールドから末尾の空白・NULを取り除いて文字列にする。
std::string read_fixed_string(const std::uint8_t* p, std::size_t n) {
    std::size_t len = n;
    while (len > 0 && (p[len - 1] == 0 || p[len - 1] == ' ')) --len;
    return std::string(reinterpret_cast<const char*>(p), len);
}

}  // namespace

const char* to_string(SerColorId id) {
    switch (id) {
        case SerColorId::Mono:      return "MONO";
        case SerColorId::BayerRGGB: return "BAYER_RGGB";
        case SerColorId::BayerGRBG: return "BAYER_GRBG";
        case SerColorId::BayerGBRG: return "BAYER_GBRG";
        case SerColorId::BayerBGGR: return "BAYER_BGGR";
        case SerColorId::BayerCYYM: return "BAYER_CYYM";
        case SerColorId::BayerYCMY: return "BAYER_YCMY";
        case SerColorId::BayerYMCY: return "BAYER_YMCY";
        case SerColorId::BayerMYYC: return "BAYER_MYYC";
        case SerColorId::RGB:       return "RGB";
        case SerColorId::BGR:       return "BGR";
    }
    return "UNKNOWN";
}

bool is_bayer(SerColorId id) {
    const std::int32_t v = static_cast<std::int32_t>(id);
    return v >= 8 && v <= 19;
}

bool is_supported_bayer(SerColorId id) {
    return id == SerColorId::BayerRGGB || id == SerColorId::BayerGRBG ||
           id == SerColorId::BayerGBRG || id == SerColorId::BayerBGGR;
}

int SerDecoder::planes() const noexcept {
    return (header_.color_id == SerColorId::RGB || header_.color_id == SerColorId::BGR) ? 3 : 1;
}

int SerDecoder::bytes_per_sample() const noexcept {
    return header_.pixel_depth <= 8 ? 1 : 2;
}

std::size_t SerDecoder::frame_bytes() const noexcept {
    return static_cast<std::size_t>(header_.width) * static_cast<std::size_t>(header_.height) *
           static_cast<std::size_t>(planes()) * static_cast<std::size_t>(bytes_per_sample());
}

int SerDecoder::effective_bit_depth() const noexcept {
    return bit_depth_override_ > 0 ? bit_depth_override_ : header_.pixel_depth;
}

bool SerDecoder::byte_order_differs_from_header() const noexcept {
    if (bytes_per_sample() == 1) return false;
    const ByteOrder claimed = header_.header_little_endian ? ByteOrder::Little : ByteOrder::Big;
    return resolved_order_ != claimed;
}

void SerDecoder::open(const std::string& path, ByteOrder order) {
    file_.open(path);

    if (file_.size() < kHeaderBytes) {
        throw std::runtime_error("SER: ファイルが小さすぎます（178バイトのヘッダに満たない）");
    }

    const std::uint8_t* h = file_.data();
    header_.file_id = read_fixed_string(h, 14);
    header_.lu_id = read_i32(h + 14);
    header_.color_id = static_cast<SerColorId>(read_i32(h + 18));
    header_.header_little_endian = read_i32(h + 22) != 0;
    header_.width = read_i32(h + 26);
    header_.height = read_i32(h + 30);
    header_.pixel_depth = read_i32(h + 34);
    header_.frame_count = read_i32(h + 38);
    header_.observer = read_fixed_string(h + 42, 40);
    header_.instrument = read_fixed_string(h + 82, 40);
    header_.telescope = read_fixed_string(h + 122, 40);
    header_.datetime = read_i64(h + 162);
    header_.datetime_utc = read_i64(h + 170);

    // SERファイル以外を掴まされた場合、ヘッダの各フィールドは事実上ランダムな値になる。
    // 上限を設けずに frame_bytes() * frame_count を計算すると容易に桁溢れし、
    // 「必要バイト数」が小さく化けてサイズ検証をすり抜けてしまう。
    // そうなると frame_stats() 等がマップ範囲外を読んでSIGSEGVで落ちる（再現確認済み）。
    // 以降の検証は乗算を使わず、上限チェックと除算で行う。
    constexpr std::int32_t kMaxDimension = 65535;  // どんなセンサーにも十分な上限

    if (header_.width <= 0 || header_.height <= 0 || header_.width > kMaxDimension ||
        header_.height > kMaxDimension) {
        throw std::runtime_error("SER: 画像サイズが不正です (" + std::to_string(header_.width) +
                                 "x" + std::to_string(header_.height) +
                                 ")。SERファイルではない可能性があります");
    }
    if (header_.pixel_depth < 1 || header_.pixel_depth > 16) {
        throw std::runtime_error("SER: ビット深度が範囲外です (" +
                                 std::to_string(header_.pixel_depth) + ", 期待値は1..16)");
    }
    if (header_.frame_count <= 0) {
        throw std::runtime_error("SER: フレーム数が不正です (" +
                                 std::to_string(header_.frame_count) + ")");
    }

    // ここまでで width*height*planes*bps は最大 65535*65535*3*2 ≈ 2.6e10 であり、
    // 64bitのsize_tに収まることが保証される。
    const std::size_t per_frame = frame_bytes();
    const std::size_t available = file_.size() - kHeaderBytes;
    const std::size_t frames = static_cast<std::size_t>(header_.frame_count);

    if (per_frame == 0 || frames > available / per_frame) {
        throw std::runtime_error(
            "SER: ファイルが途中で切れています（1フレーム " + std::to_string(per_frame) +
            " バイト × " + std::to_string(frames) + " フレームに対し、ヘッダ以降は " +
            std::to_string(available) + " バイトしかありません）");
    }

    const std::size_t frame_data_bytes = per_frame * frames;
    has_timestamps_ = (available - frame_data_bytes) >= 8u * frames;

    if (bytes_per_sample() == 1) {
        resolved_order_ = ByteOrder::Little;  // 8bitではバイトオーダーの概念がない
    } else if (order == ByteOrder::Auto) {
        resolved_order_ = detect_byte_order();
    } else {
        resolved_order_ = order;
    }
}

const std::uint8_t* SerDecoder::frame_ptr(int index) const {
    if (index < 0 || index >= header_.frame_count) {
        throw std::out_of_range("SER: フレーム番号が範囲外です (" + std::to_string(index) +
                                ", 有効範囲 0.." + std::to_string(header_.frame_count - 1) + ")");
    }
    return file_.data() + kHeaderBytes + frame_bytes() * static_cast<std::size_t>(index);
}

// バイトオーダーが正しければ画像は滑らかになり、誤っていれば隣接画素が激しく暴れる。
// 平均絶対差を両方の解釈で計算し、小さい方を採用する。差が僅かならヘッダの主張に従う。
//
// 水平方向だけを見てはいけない。画素値の増分がちょうど256の倍数に近い画像では、
// バイトを入れ替えた方が水平方向には滑らかに見えてしまう（下位バイトが変化せず
// 上位バイトだけが小刻みに増えるため）。垂直方向も同時に評価することでこれを防ぐ。
ByteOrder SerDecoder::detect_byte_order() const {
    const int sample_index = header_.frame_count / 2;
    const std::uint8_t* base = frame_ptr(sample_index);

    const int components_per_row = header_.width * planes();
    const std::size_t row_bytes = static_cast<std::size_t>(components_per_row) * 2u;

    constexpr int kMaxSampledRows = 64;
    const int row_step = header_.height > kMaxSampledRows ? header_.height / kMaxSampledRows : 1;

    double sum_little = 0.0;
    double sum_big = 0.0;
    long count = 0;

    for (int y = 0; y < header_.height; y += row_step) {
        const std::uint8_t* p = base + static_cast<std::size_t>(y) * row_bytes;

        // 水平方向（同じ行の隣の成分）
        for (int i = 0; i + 1 < components_per_row; ++i) {
            const std::uint8_t* a = p + static_cast<std::size_t>(i) * 2u;
            const std::uint8_t* b = a + 2u;
            sum_little += std::abs((b[0] | (b[1] << 8)) - (a[0] | (a[1] << 8)));
            sum_big += std::abs(((b[0] << 8) | b[1]) - ((a[0] << 8) | a[1]));
            ++count;
        }

        // 垂直方向（すぐ下の行の同じ成分）
        if (y + 1 < header_.height) {
            const std::uint8_t* q = base + static_cast<std::size_t>(y + 1) * row_bytes;
            for (int i = 0; i < components_per_row; ++i) {
                const std::uint8_t* a = p + static_cast<std::size_t>(i) * 2u;
                const std::uint8_t* b = q + static_cast<std::size_t>(i) * 2u;
                sum_little += std::abs((b[0] | (b[1] << 8)) - (a[0] | (a[1] << 8)));
                sum_big += std::abs(((b[0] << 8) | b[1]) - ((a[0] << 8) | a[1]));
                ++count;
            }
        }
    }

    const ByteOrder claimed = header_.header_little_endian ? ByteOrder::Little : ByteOrder::Big;
    if (count == 0) return claimed;

    const double little = sum_little / static_cast<double>(count);
    const double big = sum_big / static_cast<double>(count);
    const double hi = std::max(little, big);
    const double lo = std::min(little, big);
    if (hi <= 0.0 || (hi - lo) / hi < 0.05) return claimed;  // 判別がつかない
    return little < big ? ByteOrder::Little : ByteOrder::Big;
}

void SerDecoder::read_frame(int index, FrameBuffer& out) const {
    const std::uint8_t* src = frame_ptr(index);

    const int w = header_.width;
    const int h = header_.height;
    const int np = planes();
    const int bps = bytes_per_sample();
    const int depth = effective_bit_depth();

    out.reset(w, h, np);
    out.set_source_bit_depth(depth);

    const float scale = 1.0f / static_cast<float>((1u << static_cast<unsigned>(depth)) - 1u);
    const bool little = resolved_order_ == ByteOrder::Little;
    // SERのBGRは B,G,R の順で並ぶ。出力は常に plane0=R, plane1=G, plane2=B とする。
    const bool bgr = header_.color_id == SerColorId::BGR;
    const std::size_t row_bytes =
        static_cast<std::size_t>(w) * static_cast<std::size_t>(np) * static_cast<std::size_t>(bps);

    for (int y = 0; y < h; ++y) {
        const std::uint8_t* p = src + static_cast<std::size_t>(y) * row_bytes;

        if (np == 1) {
            float* d = out.row(0, y);
            if (bps == 1) {
                for (int x = 0; x < w; ++x) d[x] = static_cast<float>(p[x]) * scale;
            } else if (little) {
                for (int x = 0; x < w; ++x) {
                    const unsigned v = p[2 * x] | (static_cast<unsigned>(p[2 * x + 1]) << 8);
                    d[x] = static_cast<float>(v) * scale;
                }
            } else {
                for (int x = 0; x < w; ++x) {
                    const unsigned v = (static_cast<unsigned>(p[2 * x]) << 8) | p[2 * x + 1];
                    d[x] = static_cast<float>(v) * scale;
                }
            }
        } else {
            float* dst[3] = {out.row(bgr ? 2 : 0, y), out.row(1, y), out.row(bgr ? 0 : 2, y)};
            if (bps == 1) {
                for (int x = 0; x < w; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        dst[c][x] = static_cast<float>(p[3 * x + c]) * scale;
                    }
                }
            } else if (little) {
                for (int x = 0; x < w; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        const std::uint8_t* q = p + (static_cast<std::size_t>(3 * x + c) * 2u);
                        dst[c][x] = static_cast<float>(q[0] | (static_cast<unsigned>(q[1]) << 8)) * scale;
                    }
                }
            } else {
                for (int x = 0; x < w; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        const std::uint8_t* q = p + (static_cast<std::size_t>(3 * x + c) * 2u);
                        dst[c][x] = static_cast<float>((static_cast<unsigned>(q[0]) << 8) | q[1]) * scale;
                    }
                }
            }
        }
    }

    out.invalidate_luma();
    file_.note_read(frame_bytes());
}

FrameStats SerDecoder::frame_stats(int index) const {
    const std::uint8_t* p = frame_ptr(index);
    const std::size_t components = static_cast<std::size_t>(header_.width) *
                                   static_cast<std::size_t>(header_.height) *
                                   static_cast<std::size_t>(planes());

    FrameStats stats;
    stats.min_value = 0xFFFFFFFFu;
    stats.max_value = 0;
    double sum = 0.0;

    if (bytes_per_sample() == 1) {
        for (std::size_t i = 0; i < components; ++i) {
            const std::uint32_t v = p[i];
            stats.min_value = std::min(stats.min_value, v);
            stats.max_value = std::max(stats.max_value, v);
            sum += v;
        }
    } else {
        const bool little = resolved_order_ == ByteOrder::Little;
        for (std::size_t i = 0; i < components; ++i) {
            const std::uint8_t* q = p + i * 2u;
            const std::uint32_t v = little ? (q[0] | (static_cast<std::uint32_t>(q[1]) << 8))
                                           : ((static_cast<std::uint32_t>(q[0]) << 8) | q[1]);
            stats.min_value = std::min(stats.min_value, v);
            stats.max_value = std::max(stats.max_value, v);
            sum += v;
        }
    }

    if (components > 0) stats.mean_value = sum / static_cast<double>(components);
    else stats.min_value = 0;
    return stats;
}

std::int64_t SerDecoder::timestamp_ticks(int index) const {
    if (!has_timestamps_) {
        throw std::runtime_error("SER: このファイルにはタイムスタンプがありません");
    }
    if (index < 0 || index >= header_.frame_count) {
        throw std::out_of_range("SER: フレーム番号が範囲外です");
    }
    const std::size_t offset = kHeaderBytes +
                               frame_bytes() * static_cast<std::size_t>(header_.frame_count) +
                               8u * static_cast<std::size_t>(index);
    return read_i64(file_.data() + offset);
}

}  // namespace stackcore
