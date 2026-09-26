#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "stackcore/frame_buffer.hpp"
#include "stackcore/image_reader.hpp"

namespace stackcore {

// カメラのRAW（リニアなセンサーデータ）の読み込み。
//
// **すべて同梱の LibRaw 0.22.2（third_party/LibRaw、CDDL 1.0）で読む。** CR2・CR3・NEF・ARW・
// RAF・ORF・RW2・PEF・DNG など。LibRaw から使うのは RAW の展開と、機種ごとの切り抜き・
// 黒・白の情報だけで、色補間・色変換・ガンマ（dcraw_process）は使わない。
// （v0.3.3 までは CR2・DNG を自前で読んでいた。LibRaw と全画素一致を確かめてから一本化した）
//
// **リニアのまま渡す。** 黒レベルを引いて (値 − 黒) / (白 − 黒) で 0..1 にするだけで、
// ホワイトバランス・色変換・ガンマは掛けない（画素値が光の量に比例したままになり、
// ダーク・フラット補正とスタックが正しく効く）。色は仕上げのホワイトバランスで合わせる。
//
//   * 切り抜き: 機種の既定の範囲（CR2 は SensorInfo、DNG は DefaultCrop など。LibRaw の
//               raw_inset_crops）。無ければ LibRaw の見える範囲。LibRaw は Bayer の並びを
//               保つため、切り抜きの左上を偶数の位置に寄せることがある
//   * 黒     : LibRaw の値（遮光部・メーカーノート・BlackLevel）
//   * 白     : メーカーノートの線形の上限（ISOごとの飽和点）があればそれ、無ければ機種の最大値
//   * 並び   : Bayer は色補間せず1チャンネルで返し、ImageFileInfo::color に切り抜き後の
//               左上から見た並びを入れる。Fuji の X-Trans（6×6）は近傍平均で色補間してRGB。
//               色補間済みの DNG（LinearRaw）はRGB（またはモノ）
//   * 非対応 : 斜め配列（SuperCCD）・Foveon・CMYG、非可逆JPEG・JPEG XL で圧縮したDNG

// 拡張子（大文字小文字は区別しない）でRAWかを判定する。
bool is_raw_image_path(const std::string& path);

// 失敗時は std::runtime_error を投げる。
void read_raw_image(const std::string& path, FrameBuffer& out, ImageFileInfo& info);

// ヘッダだけを読む（画素は展開しない。ファイルはメモリに割り付けて必要な部分だけ触る）。
ImageFileInfo probe_raw_image(const std::string& path);

// EXIF形式の日時 "YYYY:MM:DD HH:MM:SS" を .NET ticks（0001-01-01 からの100ns単位）にする。
// subsec は SubSecTime の数字列（"14" なら0.14秒）、offset_minutes は UTC からのずれ
// （日本なら +540）。読めなければ 0 を返す。
std::int64_t exif_datetime_to_ticks(const std::string& datetime, const std::string& subsec,
                                    int offset_minutes);

}  // namespace stackcore
