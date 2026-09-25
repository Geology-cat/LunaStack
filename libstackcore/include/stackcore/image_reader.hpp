#pragma once

#include <string>
#include <vector>

#include "stackcore/frame_buffer.hpp"
#include "stackcore/ser_decoder.hpp"

namespace stackcore {

// 静止画1枚の読み込み（静止画連番の入力用）。
//
// 対応形式（すべて自前実装。OSのImageIOは使わない。仕様書 §8-7 の方針）:
//   * TIFF  : 8/16/32bit整数・32bit浮動小数点、1ch/3ch（アルファは捨てる）、
//             無圧縮 / LZW / Deflate / PackBits、予測子（水平差分・浮動小数点）、
//             ストリップ・タイル、チャンキー・プレーナ、リトル・ビッグエンディアン
//   * PNG   : 1〜16bit、グレー/RGB/パレット（アルファは捨てる）、非インターレース
//   * FITS  : BITPIX 8/16/32/-32/-64、2軸（モノ）または3軸（RGB面）、
//             BZERO/BSCALE、BAYERPAT、ROWORDER
//   * JPEG  : 8bitベースライン逐次
//
// 画素は 0..1 に正規化する。整数は型の最大値で割る。浮動小数点は、値が1を
// 超えるなら16bitスケールとみなして65535で割る（連番の中で枚ごとに正規化を
// 変えると明るさが揃わなくなるため、画像の中身に応じた自動調整はしない）。
struct ImageFileInfo {
    int width = 0;
    int height = 0;
    int channels = 0;          // 1 または 3
    int bit_depth = 0;         // 元データのビット深度（浮動小数点は32/64）
    SerColorId color = SerColorId::Mono;  // FITSのBAYERPATがあればBayer
    std::string format;        // "TIFF" など（表示用）
};

// 拡張子から、静止画として読める形式かを判定する（大文字小文字は区別しない）。
bool is_supported_image_path(const std::string& path);

// 失敗時は std::runtime_error を投げる。
void read_image_file(const std::string& path, FrameBuffer& out, ImageFileInfo& info);

// ヘッダだけを読む（寸法・形式の確認用）。中身の展開はしない形式もある。
ImageFileInfo probe_image_file(const std::string& path);

// ディレクトリ直下の静止画を自然順（"img2" < "img10"）で列挙する。
// 隠しファイルは含めない。
std::vector<std::string> list_image_sequence(const std::string& directory);

// 自然順の比較（数字の並びを数値として比べる）。連番の並べ替えに使う。
bool natural_less(const std::string& a, const std::string& b);

}  // namespace stackcore
