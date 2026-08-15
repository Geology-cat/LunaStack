#pragma once

// 合成SERファイル生成器。
//
// 【重要】これはリグレッション専用であり、デコーダの正しさの証明にはならない。
// 自分の書き出しと自分の読み込みが一致しているだけだからである。
// SERデコーダが正しいことは、実キャプチャソフト（FireCapture / SharpCap 等）が
// 出力した実ファイルで確認する必要がある（実装計画書 §5, M0のAC）。

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace synthetic {

struct SerSpec {
    std::string file_id = "LUCAM-RECORDER";
    int width = 32;
    int height = 24;
    int frames = 5;
    int pixel_depth = 16;
    std::int32_t color_id = 0;      // 0=MONO, 8=RGGB, 100=RGB
    bool header_says_little = true; // ヘッダに書き込む LittleEndian フラグ
    bool data_is_little = true;     // 実データの並び（ヘッダとわざと食い違わせられる）
    bool with_timestamps = true;
    // 実際の撮像には必ずノイズが乗るため既定で付加する。
    // false にすると「増分がちょうど256の倍数になる無ノイズのランプ」という
    // バイトオーダー自動判定にとって最悪のケースを再現できる。
    bool with_noise = true;
};

namespace detail {

// 決定論的な擬似乱数（同じ座標からは常に同じ値が出る）。
inline std::uint32_t hash_u32(std::uint32_t v) {
    v ^= v >> 16;
    v *= 0x7feb352dU;
    v ^= v >> 15;
    v *= 0x846ca68bU;
    v ^= v >> 16;
    return v;
}

}  // namespace detail

// 水平・垂直方向に滑らかなランプ画像＋ノイズ。
// バイトオーダー判定は「正しい並びなら画像が滑らかになる」ことを利用するため、
// テストデータも実画像と同様に滑らかかつノイズを含む必要がある。
inline std::uint32_t synthetic_value(const SerSpec& spec, int x, int y, int frame,
                                     int component) {
    const std::uint32_t max_value = (1u << static_cast<unsigned>(spec.pixel_depth)) - 1u;
    const double fx = static_cast<double>(x) / (spec.width > 1 ? spec.width - 1 : 1);
    const double fy = static_cast<double>(y) / (spec.height > 1 ? spec.height - 1 : 1);
    double v = 0.15 + 0.5 * fx + 0.2 * fy + 0.02 * frame + 0.03 * component;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;

    std::int64_t value = static_cast<std::int64_t>(v * static_cast<double>(max_value) + 0.5);

    if (spec.with_noise) {
        std::uint32_t amplitude = max_value / 512u;
        if (amplitude == 0u) amplitude = 1u;
        const std::uint32_t h = detail::hash_u32(
            static_cast<std::uint32_t>(x * 73856093) ^
            static_cast<std::uint32_t>(y * 19349663) ^
            static_cast<std::uint32_t>((frame * 8 + component) * 83492791));
        value += static_cast<std::int64_t>(h % (2u * amplitude + 1u)) -
                 static_cast<std::int64_t>(amplitude);
    }

    if (value < 0) value = 0;
    if (value > static_cast<std::int64_t>(max_value)) value = max_value;
    return static_cast<std::uint32_t>(value);
}

namespace detail {

inline void push_i32(std::vector<std::uint8_t>& b, std::int32_t v) {
    const std::uint32_t u = static_cast<std::uint32_t>(v);
    b.push_back(static_cast<std::uint8_t>(u & 0xFF));
    b.push_back(static_cast<std::uint8_t>((u >> 8) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((u >> 16) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((u >> 24) & 0xFF));
}

inline void push_i64(std::vector<std::uint8_t>& b, std::int64_t v) {
    const std::uint64_t u = static_cast<std::uint64_t>(v);
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>((u >> (8 * i)) & 0xFF));
}

inline void push_fixed(std::vector<std::uint8_t>& b, const std::string& s, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        b.push_back(i < s.size() ? static_cast<std::uint8_t>(s[i]) : 0);
    }
}

}  // namespace detail

// 指定パスに合成SERを書き出す。
inline void write_ser(const std::string& path, const SerSpec& spec) {
    const int planes = (spec.color_id == 100 || spec.color_id == 101) ? 3 : 1;
    const int bps = spec.pixel_depth <= 8 ? 1 : 2;

    std::vector<std::uint8_t> out;

    detail::push_fixed(out, spec.file_id, 14);
    detail::push_i32(out, 0);                       // LuID
    detail::push_i32(out, spec.color_id);
    detail::push_i32(out, spec.header_says_little ? 1 : 0);
    detail::push_i32(out, spec.width);
    detail::push_i32(out, spec.height);
    detail::push_i32(out, spec.pixel_depth);
    detail::push_i32(out, spec.frames);
    detail::push_fixed(out, "TestObserver", 40);
    detail::push_fixed(out, "TestCamera", 40);
    detail::push_fixed(out, "TestScope", 40);
    const std::int64_t ticks = 638000000000000000LL;  // 2022年ごろ
    detail::push_i64(out, ticks);
    detail::push_i64(out, ticks);

    if (out.size() != 178) {
        throw std::logic_error("合成SER: ヘッダ長が178バイトになっていません");
    }

    for (int f = 0; f < spec.frames; ++f) {
        for (int y = 0; y < spec.height; ++y) {
            for (int x = 0; x < spec.width; ++x) {
                for (int c = 0; c < planes; ++c) {
                    const std::uint32_t v = synthetic_value(spec, x, y, f, c);
                    if (bps == 1) {
                        out.push_back(static_cast<std::uint8_t>(v & 0xFF));
                    } else if (spec.data_is_little) {
                        out.push_back(static_cast<std::uint8_t>(v & 0xFF));
                        out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
                    } else {
                        out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
                        out.push_back(static_cast<std::uint8_t>(v & 0xFF));
                    }
                }
            }
        }
    }

    if (spec.with_timestamps) {
        for (int f = 0; f < spec.frames; ++f) {
            detail::push_i64(out, ticks + static_cast<std::int64_t>(f) * 100000LL);
        }
    }

    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (fp == nullptr) throw std::runtime_error("合成SER: ファイルを作成できません: " + path);
    const std::size_t written = std::fwrite(out.data(), 1, out.size(), fp);
    std::fclose(fp);
    if (written != out.size()) throw std::runtime_error("合成SER: 書き込みに失敗しました");
}

// ファイルを途中で切り詰めたコピーを作る（破損ファイルの検出テスト用）。
inline void write_truncated(const std::string& src, const std::string& dst,
                            std::size_t keep_bytes) {
    std::FILE* in = std::fopen(src.c_str(), "rb");
    if (in == nullptr) throw std::runtime_error("合成SER: 入力を開けません: " + src);
    std::vector<std::uint8_t> buf(keep_bytes);
    const std::size_t got = std::fread(buf.data(), 1, keep_bytes, in);
    std::fclose(in);

    std::FILE* out = std::fopen(dst.c_str(), "wb");
    if (out == nullptr) throw std::runtime_error("合成SER: 出力を作成できません: " + dst);
    std::fwrite(buf.data(), 1, got, out);
    std::fclose(out);
}

}  // namespace synthetic
