#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "stackcore/calibration.hpp"
#include "stackcore/debayer.hpp"
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

    // Bayer入力をどの方式でデバイヤーするか（read_prepared_frame が使う）。
    virtual DebayerMethod debayer_method() const { return DebayerMethod::Bilinear; }

    // このソースでのフレーム番号 → 元のファイルでのフレーム番号。
    // フレーム範囲を指定したときに、画面には元の番号を出すために使う。
    virtual int original_index(int index) const { return index; }
};

struct OpenOptions {
    ByteOrder endian = ByteOrder::Auto;  // SERのみ意味を持つ
    int bit_depth_override = 0;          // SERのみ意味を持つ

    // ---- 入力の前処理（すべて既定値なら元のソースをそのまま返す） ----
    // 使うフレームの範囲 [frame_start, frame_end)。frame_end が 0 なら最後まで。
    int frame_start = 0;
    int frame_end = 0;
    // 色形式の手動指定。ヘッダが Bayer を名乗らないモノクロ扱いのファイル
    // （静止画やPIPP出力など）や、ヘッダの配列が誤っているファイルへの対処。
    bool override_color = false;
    SerColorId color_override = SerColorId::Mono;
    DebayerMethod debayer = DebayerMethod::Bilinear;
    // ダーク・フラット補正。null なら補正しない。
    std::shared_ptr<const CalibrationFrames> calibration;
    // 静止画連番。空でなければ path の代わりにこれらのファイルを順に読む。
    // path がフォルダのときは、その直下の静止画を自然順に並べて使う。
    std::vector<std::string> sequence_files;
    // 処理範囲（入力の画素座標）。幅か高さが0なら全体。品質評価からスタックまで、この範囲だけを
    // 読んだものとして扱う（大きなセンサーの中央にだけ写っている対象を速く・少ないメモリで処理する）。
    // ダーク・フラット補正は全体に掛けてから切り出すので、マスターは全体の大きさのままでよい。
    // 左上は偶数に切り下げ、幅・高さも偶数にそろえる（Bayer の並びを変えないため。effective_roi）。
    int roi_x = 0;
    int roi_y = 0;
    int roi_width = 0;
    int roi_height = 0;
    bool has_roi() const { return roi_width > 0 && roi_height > 0; }

    bool has_preprocessing() const {
        return frame_start != 0 || frame_end != 0 || override_color ||
               debayer != DebayerMethod::Bilinear || (calibration && !calibration->empty()) ||
               has_roi();
    }
};

// 処理範囲を、入力の寸法 width×height の中に収め、左上と大きさを偶数にそろえた値。
// 範囲の指定が無ければ全体を返す。
void effective_roi(const OpenOptions& options, int width, int height, int& x, int& y, int& w, int& h);

// 拡張子と中身から形式を判定して開く。失敗時は std::runtime_error を投げる。
//
// **入力はすべてここを通すこと。** 前処理（範囲・補正・色形式）を掛けた
// ソースを返すので、ここを迂回すると一部の画面だけ補正されていない、という
// 食い違いが起きる。前処理がすべて既定値なら元のソースをそのまま返し、
// 出力はv0.2系とバイト単位で一致する。
std::unique_ptr<VideoSource> open_video(const std::string& path, const OpenOptions& options);

// 前処理を掛けない素のソースを開く（マスターダーク・フラットの作成用）。
std::unique_ptr<VideoSource> open_raw_video(const std::string& path, const OpenOptions& options);

// path がフォルダか（静止画連番として開く対象か）。
bool is_directory_path(const std::string& path);

// 拡張子で MOV・MP4・M4V（AVFoundation で読む動画）かを判定する。
bool is_movie_path(const std::string& path);

}  // namespace stackcore
