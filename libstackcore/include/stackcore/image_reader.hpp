#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "stackcore/frame_buffer.hpp"
#include "stackcore/ser_decoder.hpp"

namespace stackcore {

// 静止画1枚の読み込み（静止画連番の入力用）。
//
// 対応形式（OSのImageIOは使わない。仕様書 §8-7 の方針。RAW以外は自前実装）:
//   * TIFF  : 8/16/32bit整数・32bit浮動小数点、1ch/3ch（アルファは捨てる）、
//             無圧縮 / LZW / Deflate / PackBits、予測子（水平差分・浮動小数点）、
//             ストリップ・タイル、チャンキー・プレーナ、リトル・ビッグエンディアン
//   * PNG   : 1〜16bit、グレー/RGB/パレット（アルファは捨てる）、非インターレース
//   * FITS  : BITPIX 8/16/32/-32/-64、2軸（モノ）または3軸（RGB面）、
//             BZERO/BSCALE、BAYERPAT、ROWORDER
//   * JPEG  : 8bitベースライン逐次
//   * RAW   : CR2・CR3・NEF・ARW・RAF・DNG など（raw_reader.hpp。同梱の LibRaw で読む。
//             リニアのまま、Bayer は1チャンネルで返す）
//
// 画素は 0..1 に正規化する。整数は型の最大値で割る。浮動小数点は、値が1を
// 超えるなら16bitスケールとみなして65535で割る（連番の中で枚ごとに正規化を
// 変えると明るさが揃わなくなるため、画像の中身に応じた自動調整はしない）。
struct ImageFileInfo {
    int width = 0;
    int height = 0;
    int channels = 0;          // 1 または 3
    int bit_depth = 0;         // 元データのビット深度（浮動小数点は32/64）
    SerColorId color = SerColorId::Mono;  // FITSのBAYERPAT・RAWのCFAならBayer
    std::string format;        // "TIFF" など（表示用）
    std::string camera;        // 撮影したカメラ（RAWのみ。例 "Canon EOS 6D Mark II"）
    // 撮影時刻（UTC、.NET ticks = 0001-01-01 からの100ns単位）。RAWで、時差まで
    // 分かるときだけ入る。分からなければ0（現地時刻をUTCと取り違えないため）。
    std::int64_t timestamp_ticks = 0;
};

// 拡張子から、静止画として読める形式かを判定する（大文字小文字は区別しない）。
bool is_supported_image_path(const std::string& path);

// 失敗時は std::runtime_error を投げる。
void read_image_file(const std::string& path, FrameBuffer& out, ImageFileInfo& info);

// ヘッダだけを読む（寸法・形式の確認用）。中身の展開はしない形式もある。
ImageFileInfo probe_image_file(const std::string& path);

// 連番に使う画像を1種類にそろえる。カメラは RAW と JPEG を同時に保存することが多く、
// 混ぜると先頭の1枚（CFA）と JPEG（RGB）で形式が食い違って途中で止まる。
// RAWがあれば、いちばん多いRAWの形式だけ。無ければ、いちばん多い形式だけ
// （tif/tiff・jpg/jpeg・fit/fits/fts は同じ形式とみなす。同数なら先に出てくる方）。
// 順番は保つ。
std::vector<std::string> select_sequence_files(const std::vector<std::string>& paths);

// ディレクトリ直下の静止画を自然順（"img2" < "img10"）で列挙する。
// 隠しファイルは含めない。形式は select_sequence_files で1種類にそろえる。
std::vector<std::string> list_image_sequence(const std::string& directory);

// 自然順の比較（数字の並びを数値として比べる）。連番の並べ替えに使う。
bool natural_less(const std::string& a, const std::string& b);

}  // namespace stackcore
