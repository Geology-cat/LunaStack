#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "stackcore/frame_buffer.hpp"
#include "stackcore/ser_decoder.hpp"

namespace stackcore {

// 動画入力の共通インターフェース。
//
// パイプライン（品質評価・アライメント・スタック）は入力の形式を知らなくてよい。
// 形式ごとの癖（SERのバイトオーダー、AVIの上下規約など）は
// この層より下で吸収し、上流には 0..1 正規化済みの FrameBuffer だけを渡す。
class VideoSource {
public:
    virtual ~VideoSource() = default;

    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual int frame_count() const = 0;

    // 色形式。表現は SER の ColorID を共通の型として流用している。
    virtual SerColorId color_id() const = 0;

    // 正規化に使ったビット深度。
    virtual int bit_depth() const = 0;

    virtual bool has_timestamps() const = 0;
    // has_timestamps() が false のときの値は未定義（0を返す）。
    virtual std::int64_t timestamp_ticks(int index) const = 0;

    virtual void read_frame(int index, FrameBuffer& out) const = 0;
    virtual FrameStats frame_stats(int index) const = 0;

    // 形式固有の情報を1行で表す（CLIの表示用）。
    virtual std::string describe() const = 0;
    virtual const char* format_name() const = 0;

    // 自動判定したバイトオーダーが、ファイルのヘッダの主張と食い違うか。
    //
    // 食い違いは即座に誤りを意味しないが、画像が破綻して見えるときに
    // まず疑うべき場所である。GUIはこれを見て警告を出す（UI設計書 §7.2）。
    // 形式によっては意味を持たない（AVIなど）ので、既定は false。
    virtual bool byte_order_suspect() const { return false; }

    // 低メモリモード（仕様書 §7.3 の「設定で2GBまで引き下げ可能」）。
    //
    // 有効にすると、読み終えたフレームのページをカーネルに返す。
    // mmapのページはファイルバックドなので放っておいても破綻はしないが、
    // 最大RSSがファイルサイズまで膨らむ。2.6GBのSERで実測2.86GBに達した。
    // 対応する形式でだけ意味を持つ（既定の実装は何もしない）。
    virtual void set_low_memory(bool) {}

    // 同じ入力を複数スレッドから同時に読めるか。
    // 通常の読み取り専用mmapは安全だが、低メモリモードでは読み進める途中で
    // マッピングを張り直すため、並列読み取りを禁止しなければならない。
    virtual bool supports_concurrent_reads() const { return true; }
};

struct OpenOptions {
    ByteOrder endian = ByteOrder::Auto;  // SERのみ意味を持つ
    int bit_depth_override = 0;          // SERのみ意味を持つ
};

// 拡張子と中身から形式を判定して開く。失敗時は std::runtime_error を投げる。
std::unique_ptr<VideoSource> open_video(const std::string& path, const OpenOptions& options);

}  // namespace stackcore
