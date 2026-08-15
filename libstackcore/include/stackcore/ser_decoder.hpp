#pragma once

#include <cstdint>
#include <string>

#include "stackcore/frame_buffer.hpp"
#include "stackcore/mapped_file.hpp"

namespace stackcore {

enum class SerColorId : std::int32_t {
    Mono = 0,
    BayerRGGB = 8,
    BayerGRBG = 9,
    BayerGBRG = 10,
    BayerBGGR = 11,
    BayerCYYM = 16,
    BayerYCMY = 17,
    BayerYMCY = 18,
    BayerMYYC = 19,
    RGB = 100,
    BGR = 101,
};

const char* to_string(SerColorId id);
bool is_bayer(SerColorId id);
bool is_supported_bayer(SerColorId id);  // RGGB/GRBG/GBRG/BGGR のみ true

// 16bitサンプルのバイトオーダー。
//
// SERヘッダの LittleEndian フラグはキャプチャソフト間で解釈が割れており信用できない。
// 誤るとサンプルのバイトが逆転して画像がノイズにしか見えなくなるため、既定は
// 自動判定とし、手動上書きを常に用意する（実装計画書 §4.8）。
enum class ByteOrder { Auto, Little, Big };

struct SerHeader {
    std::string file_id;                 // 通常 "LUCAM-RECORDER"
    std::int32_t lu_id = 0;
    SerColorId color_id = SerColorId::Mono;
    bool header_little_endian = false;   // ヘッダの主張（判定には使うが盲信しない）
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t pixel_depth = 0;        // 1..16 bit/plane
    std::int32_t frame_count = 0;
    std::string observer;
    std::string instrument;
    std::string telescope;
    std::int64_t datetime = 0;           // .NET ticks（0001-01-01 からの100ns単位）
    std::int64_t datetime_utc = 0;
};

// 生サンプル値の統計。16bitコンテナに12bitデータが入っている等の診断に使う。
struct FrameStats {
    std::uint32_t min_value = 0;
    std::uint32_t max_value = 0;
    double mean_value = 0.0;
};

class SerDecoder {
public:
    // 失敗時は std::runtime_error を投げる。
    void open(const std::string& path, ByteOrder order = ByteOrder::Auto);

    const SerHeader& header() const noexcept { return header_; }
    int planes() const noexcept;             // RGB/BGR なら3、それ以外は1
    int bytes_per_sample() const noexcept;   // 深度8以下なら1、それ以外は2
    std::size_t frame_bytes() const noexcept;
    int frame_count() const noexcept { return header_.frame_count; }

    bool has_timestamps() const noexcept { return has_timestamps_; }
    std::int64_t timestamp_ticks(int index) const;

    ByteOrder resolved_byte_order() const noexcept { return resolved_order_; }
    // 自動判定の結果がヘッダの主張と食い違ったか（食い違いは異常ではなく実際によくある）。
    bool byte_order_differs_from_header() const noexcept;

    // 正規化に使うビット深度を明示的に上書きする（0で解除）。
    // 「16bitと書いてあるが中身は12bit」というファイルへの対処手段。
    void set_bit_depth_override(int bits) noexcept { bit_depth_override_ = bits; }
    int effective_bit_depth() const noexcept;

    // フレームを 0..1 に正規化して読み出す。デバイヤーは行わない
    // （Bayerの場合は1chのCFA画像がそのまま入る）。
    void read_frame(int index, FrameBuffer& out) const;

    FrameStats frame_stats(int index) const;

    // 低メモリモード用。budget バイト読むごとにマッピングを張り直す。
    void advise_sequential() const noexcept { file_.advise_sequential(); }
    void set_reclaim_budget(std::size_t bytes) { file_.set_reclaim_budget(bytes); }

private:
    const std::uint8_t* frame_ptr(int index) const;
    ByteOrder detect_byte_order() const;

    MappedFile file_;
    SerHeader header_;
    bool has_timestamps_ = false;
    ByteOrder resolved_order_ = ByteOrder::Little;
    int bit_depth_override_ = 0;
};

}  // namespace stackcore
