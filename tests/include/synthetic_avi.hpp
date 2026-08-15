#pragma once

// テスト用の最小AVIライター。
//
// 位置づけに注意: これは**回帰テストと分岐網羅のための道具**であって、
// デコーダの正しさの証明ではない。自作writerと自作readerが一致しても
// それは自己一致にすぎない（仕様書 §2.3）。
// デコーダが実際に正しいことは、ffmpegのデコード結果とのバイト一致
// （合成AVI 4種）と、PIPPが書いた実AVIでの確認によって担保している。
//
// ここで作るのは、ffmpegでは作り分けられない構造:
//   * biHeightの符号（上から下 / 下から上）
//   * FourCC形式での上下規約
//   * OpenDML(AVI 2.0)の複数RIFFセグメント
//   * movi内の 'rec ' による束ね
//   * 行のパディング

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace synthetic_avi {

struct Options {
    int width = 16;
    int height = 12;
    int bit_count = 24;
    std::uint32_t compression = 0;  // 0=BI_RGB、それ以外はFourCC
    bool negative_height = false;   // biHeightを負にする（上から下）
    bool pad_rows = true;           // 行をDWORD境界に切り上げる
    bool group_in_rec = false;      // フレームを 'rec ' でまとめる
    int frames_per_segment = 0;     // >0 で OpenDML の複数RIFFに分割する
};

inline std::uint32_t fourcc(const char* s) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[0])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[3])) << 24);
}

namespace detail {

inline void put_u32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 24) & 0xFF));
}

inline void put_i32(std::vector<std::uint8_t>& v, std::int32_t x) {
    put_u32(v, static_cast<std::uint32_t>(x));
}

inline void put_u16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
}

inline void put_cc(std::vector<std::uint8_t>& v, const char* s) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(s[i]));
}

inline void patch_u32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
    v[at] = static_cast<std::uint8_t>(x & 0xFF);
    v[at + 1] = static_cast<std::uint8_t>((x >> 8) & 0xFF);
    v[at + 2] = static_cast<std::uint8_t>((x >> 16) & 0xFF);
    v[at + 3] = static_cast<std::uint8_t>((x >> 24) & 0xFF);
}

}  // namespace detail

// 画素の期待値。テスト側でも同じ式を使って突き合わせる。
// frame / x / y のすべてに依存させ、行や列の取り違えを検出できるようにする。
inline int expected_sample(int frame, int x, int y, int channel) {
    return (x * 3 + y * 7 + frame * 11 + channel * 29) % 251;
}

// 1フレーム分の画素データを、AVIに書く並び（bottom-upなら下の行から）で作る。
inline std::vector<std::uint8_t> frame_payload(const Options& o, int frame,
                                               std::size_t row_bytes, bool top_down) {
    const int channels = o.bit_count >= 24 ? 3 : 1;
    const int step = o.bit_count / 8;
    std::vector<std::uint8_t> data(row_bytes * static_cast<std::size_t>(o.height), 0);

    for (int y = 0; y < o.height; ++y) {
        const int stored_y = top_down ? y : (o.height - 1 - y);
        std::uint8_t* row = data.data() + static_cast<std::size_t>(stored_y) * row_bytes;
        for (int x = 0; x < o.width; ++x) {
            std::uint8_t* px = row + static_cast<std::size_t>(x) * step;
            if (channels == 1) {
                if (o.bit_count == 16) {
                    const int v = expected_sample(frame, x, y, 0) * 257;
                    px[0] = static_cast<std::uint8_t>(v & 0xFF);
                    px[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
                } else {
                    px[0] = static_cast<std::uint8_t>(expected_sample(frame, x, y, 0));
                }
            } else {
                // DIBはメモリ上 B, G, R の順。channel 0=R, 1=G, 2=B とする。
                px[0] = static_cast<std::uint8_t>(expected_sample(frame, x, y, 2));
                px[1] = static_cast<std::uint8_t>(expected_sample(frame, x, y, 1));
                px[2] = static_cast<std::uint8_t>(expected_sample(frame, x, y, 0));
            }
        }
    }
    return data;
}

inline std::size_t row_bytes_of(const Options& o) {
    const std::size_t bits = static_cast<std::size_t>(o.width) * o.bit_count;
    return o.pad_rows ? ((bits + 31) / 32) * 4 : (bits + 7) / 8;
}

// AVIファイルをバイト列として組み立てる。
inline std::vector<std::uint8_t> build(const Options& o, int frame_count) {
    using namespace detail;
    const std::size_t row_bytes = row_bytes_of(o);
    const std::size_t frame_size = row_bytes * static_cast<std::size_t>(o.height);
    // 読み手が上から下として扱うべきか。デコーダと同じ規約で書く。
    const bool top_down = o.negative_height || o.compression != 0;

    std::vector<std::uint8_t> v;

    // ---- RIFF 'AVI ' -----------------------------------------------------
    put_cc(v, "RIFF");
    const std::size_t riff_size_at = v.size();
    put_u32(v, 0);
    put_cc(v, "AVI ");

    // ---- LIST 'hdrl' -----------------------------------------------------
    put_cc(v, "LIST");
    const std::size_t hdrl_size_at = v.size();
    put_u32(v, 0);
    const std::size_t hdrl_begin = v.size();
    put_cc(v, "hdrl");

    put_cc(v, "avih");
    put_u32(v, 56);
    for (int i = 0; i < 56; ++i) v.push_back(0);

    put_cc(v, "LIST");
    const std::size_t strl_size_at = v.size();
    put_u32(v, 0);
    const std::size_t strl_begin = v.size();
    put_cc(v, "strl");

    put_cc(v, "strh");
    put_u32(v, 56);
    const std::size_t strh_begin = v.size();
    put_cc(v, "vids");
    put_u32(v, o.compression);  // fccHandler
    while (v.size() < strh_begin + 20) v.push_back(0);
    put_u32(v, 1);   // dwScale
    put_u32(v, 30);  // dwRate → 30fps
    while (v.size() < strh_begin + 56) v.push_back(0);

    put_cc(v, "strf");
    put_u32(v, 40);
    put_u32(v, 40);  // biSize
    put_i32(v, o.width);
    put_i32(v, o.negative_height ? -o.height : o.height);
    put_u16(v, 1);                                       // biPlanes
    put_u16(v, static_cast<std::uint16_t>(o.bit_count));  // biBitCount
    put_u32(v, o.compression);
    put_u32(v, static_cast<std::uint32_t>(frame_size));  // biSizeImage
    for (int i = 0; i < 16; ++i) v.push_back(0);

    patch_u32(v, strl_size_at, static_cast<std::uint32_t>(v.size() - strl_begin));
    patch_u32(v, hdrl_size_at, static_cast<std::uint32_t>(v.size() - hdrl_begin));

    // ---- movi ------------------------------------------------------------
    const int per_segment =
        o.frames_per_segment > 0 ? o.frames_per_segment : frame_count;
    int written = 0;
    bool first_segment = true;

    while (written < frame_count) {
        const int n = (frame_count - written) < per_segment ? (frame_count - written)
                                                            : per_segment;
        if (!first_segment) {
            // OpenDML の追加セグメント。RIFF 'AVIX' + LIST 'movi'。
            put_cc(v, "RIFF");
            const std::size_t avix_size_at = v.size();
            put_u32(v, 0);
            const std::size_t avix_begin = v.size();
            put_cc(v, "AVIX");

            put_cc(v, "LIST");
            const std::size_t movi_size_at = v.size();
            put_u32(v, 0);
            const std::size_t movi_begin = v.size();
            put_cc(v, "movi");
            for (int i = 0; i < n; ++i) {
                const std::vector<std::uint8_t> payload =
                    frame_payload(o, written + i, row_bytes, top_down);
                put_cc(v, "00dc");
                put_u32(v, static_cast<std::uint32_t>(payload.size()));
                v.insert(v.end(), payload.begin(), payload.end());
                if (payload.size() & 1) v.push_back(0);
            }
            patch_u32(v, movi_size_at, static_cast<std::uint32_t>(v.size() - movi_begin));
            patch_u32(v, avix_size_at, static_cast<std::uint32_t>(v.size() - avix_begin));
        } else {
            put_cc(v, "LIST");
            const std::size_t movi_size_at = v.size();
            put_u32(v, 0);
            const std::size_t movi_begin = v.size();
            put_cc(v, "movi");

            if (o.group_in_rec) {
                put_cc(v, "LIST");
                const std::size_t rec_size_at = v.size();
                put_u32(v, 0);
                const std::size_t rec_begin = v.size();
                put_cc(v, "rec ");
                for (int i = 0; i < n; ++i) {
                    const std::vector<std::uint8_t> payload =
                        frame_payload(o, written + i, row_bytes, top_down);
                    put_cc(v, "00dc");
                    put_u32(v, static_cast<std::uint32_t>(payload.size()));
                    v.insert(v.end(), payload.begin(), payload.end());
                    if (payload.size() & 1) v.push_back(0);
                }
                patch_u32(v, rec_size_at, static_cast<std::uint32_t>(v.size() - rec_begin));
            } else {
                for (int i = 0; i < n; ++i) {
                    const std::vector<std::uint8_t> payload =
                        frame_payload(o, written + i, row_bytes, top_down);
                    put_cc(v, "00dc");
                    put_u32(v, static_cast<std::uint32_t>(payload.size()));
                    v.insert(v.end(), payload.begin(), payload.end());
                    if (payload.size() & 1) v.push_back(0);
                }
            }
            patch_u32(v, movi_size_at, static_cast<std::uint32_t>(v.size() - movi_begin));
            // 先頭セグメントのRIFFサイズはここで確定する。
            patch_u32(v, riff_size_at, static_cast<std::uint32_t>(v.size() - 8));
        }
        written += n;
        first_segment = false;
    }

    if (frame_count == 0) {
        patch_u32(v, riff_size_at, static_cast<std::uint32_t>(v.size() - 8));
    }
    return v;
}

// 一時ファイルに書き出してパスを返す。
inline std::string write_temp(const std::vector<std::uint8_t>& bytes, const char* name) {
    std::string path = std::string("/tmp/lunastack_test_") + name + ".avi";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return std::string();
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return path;
}

}  // namespace synthetic_avi
