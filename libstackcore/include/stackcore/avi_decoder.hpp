#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "stackcore/frame_buffer.hpp"
#include "stackcore/mapped_file.hpp"
#include "stackcore/ser_decoder.hpp"

namespace stackcore {

// AVI（非圧縮 / MJPEG）の自前デコーダ。
//
// なぜOS付属のデコーダ（AVFoundation）を使わないか:
// 「同一入力・同一設定なら全OSバージョンで同一の品質」という要件があり、
// AVFoundationのAVI対応はOSバージョンで挙動が変わりうるため
// 主要フォーマットから排除している（仕様書 §3.1・§8-7）。
//
// 対応する範囲:
//   * AVI 1.0 と OpenDML(AVI 2.0)。2GBを超えるファイルは後者になり、
//     movi が複数の RIFF 'AVIX' セグメントに分割される
//   * 非圧縮 BI_RGB (8 / 24 / 32 bit) と、Y800 / GREY / Y8 / Y16 などの
//     生画素 FourCC
//   * MJPG / JPEG / dmb1 の8bitベースラインJPEG
class AviDecoder {
public:
    struct Header {
        int width = 0;
        int height = 0;
        // DIBは既定で「下から上」に格納される。BITMAPINFOHEADERのbiHeightが
        // 負のときだけ「上から下」になる。ffmpegは bgr24 を負、Y800 を正で
        // 書くことを確認しており、符号を無視すると上下が反転する。
        bool top_down = false;
        int bit_count = 0;
        std::uint32_t compression = 0;  // BI_RGB(0) または FourCC
        std::string compression_name;   // 表示用（"BI_RGB" / "Y800" 等）
        std::string handler;            // strh の fccHandler
        int frame_count = 0;
        double fps = 0.0;
        // 1行あたりの実バイト数。DIBはDWORD境界に揃えるのが本来だが、
        // 揃えない書き手もいるため、実際のチャンクサイズから決める。
        std::size_t row_bytes = 0;
    };

    // 失敗時は std::runtime_error を投げる。
    void open(const std::string& path);

    const Header& header() const noexcept { return header_; }
    int frame_count() const noexcept { return static_cast<int>(frames_.size()); }
    int planes() const noexcept;
    int bytes_per_sample() const noexcept;
    int bit_depth() const noexcept;
    bool is_mjpeg() const noexcept { return mjpeg_; }

    // パイプラインの他の段（デバイヤー等）と型を揃えるため、
    // 色形式の表現には SER の ColorID を流用する。
    SerColorId color_id() const noexcept { return color_id_; }

    std::size_t frame_bytes(int index) const;

    // フレームを 0..1 に正規化して読み出す。上下の向きはここで正す。
    void read_frame(int index, FrameBuffer& out) const;

    FrameStats frame_stats(int index) const;

    // 低メモリモード用。budget バイト読むごとにマッピングを張り直す。
    void advise_sequential() const noexcept { file_.advise_sequential(); }
    void set_reclaim_budget(std::size_t bytes) { file_.set_reclaim_budget(bytes); }

private:
    struct FrameRef {
        std::uint64_t offset = 0;  // 画素データの先頭（チャンクヘッダの次）
        std::uint32_t size = 0;
    };

    void parse(const std::uint8_t* data, std::uint64_t size);
    void scan_movi(const std::uint8_t* data, std::uint64_t begin, std::uint64_t end);

    MappedFile file_;
    Header header_;
    std::vector<FrameRef> frames_;
    SerColorId color_id_ = SerColorId::Mono;
    int video_stream_ = 0;
    bool mjpeg_ = false;
};

}  // namespace stackcore
