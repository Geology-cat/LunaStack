#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "stackcore/frame_buffer.hpp"
#include "stackcore/image_reader.hpp"

namespace stackcore {

// カメラのRAW（リニアなセンサーデータ）の読み込み。CR2・DNG は自前実装（仕様書 §8-7）、そのほかは LibRaw。
//
// 対応形式:
//   * Canon CR2 : 自前。ロスレスJPEG（スライス分割あり・なし）。sRAW/mRAW は非対応
//   * DNG       : CFA（2×2のBayer）と LinearRaw（色補間済みのリニアRGB・モノ）。
//                 無圧縮（8/16bit・詰めた10/12/14bit・16/24/32bit浮動小数点）、
//                 ロスレスJPEG、Deflate（予測子なし・水平差分・浮動小数点）、
//                 ストリップ・タイル、LinearizationTable、ActiveArea・DefaultCrop、
//                 BlackLevel（繰り返し・行列の差分つき）・WhiteLevel。
//                 非可逆JPEG・JPEG XL のDNGは非対応（自前）
//   * そのほか  : CR3・NEF・ARW・RAF・ORF・RW2・PEF など（LibRaw 0.22.2 で展開。
//                 third_party/LibRaw）。Bayer は1チャンネルのまま、Fuji の X-Trans（6×6）は
//                 近傍平均で色補間してRGBで返す。斜め配列（SuperCCD）・Foveon・CMYG は非対応
//
// **リニアのまま渡す。** 黒レベルを引いて (値 − 黒) / (白 − 黒) で 0..1 にするだけで、
// ホワイトバランス・色変換・ガンマは掛けない（画素値が光の量に比例したままになり、
// ダーク・フラット補正とスタックが正しく効く）。色は仕上げのホワイトバランスで合わせる。
//
// CFAはデバイヤーせず1チャンネルのまま返し、ImageFileInfo::color に切り抜き後の
// 左上から見たBayerの並びを入れる（奇数画素ぶん切り抜くと並びが変わるため計算し直す）。
// 切り抜く範囲:
//   * CR2 : メーカーノートの SensorInfo（左・上・右・下の境界）。遮光部を除いた範囲
//   * DNG : ActiveArea の中の DefaultCrop
// 黒レベル:
//   * CR2 : 左の遮光部の平均（Bayerの位相ごと）
//   * DNG : BlackLevel（＋BlackLevelDeltaH/V）
// 白レベル:
//   * CR2 : ISOごとの飽和点（メーカーノート。LibRaw で読む）。分からなければ記録ビット数の最大値
//   * DNG : WhiteLevel
//   * LibRaw の形式: 黒・白とも LibRaw の値（白はメーカーノートの線形の上限があればそれ）

// 拡張子（大文字小文字は区別しない）でRAWかを判定する。
bool is_raw_image_path(const std::string& path);

// 失敗時は std::runtime_error を投げる。
void read_raw_image(const std::string& path, FrameBuffer& out, ImageFileInfo& info);

// ヘッダだけを読む（画素は展開しない。ファイルはメモリに割り付けて必要な部分だけ触る）。
ImageFileInfo probe_raw_image(const std::string& path);

// ---- 照合・テスト用 ----------------------------------------------------------

// 切り抜き・黒レベル補正の前の、センサーの生の値（1画素1標本のCFA）。
struct RawSensorData {
    int width = 0;
    int height = 0;
    std::vector<std::uint16_t> values;  // 行優先、width*height 個
    int crop_x = 0, crop_y = 0, crop_width = 0, crop_height = 0;
    double black[4] = {0, 0, 0, 0};  // 生の座標での位相 (y&1)*2+(x&1) ごと
    double white = 0.0;
    int bits = 0;
};

// CR2 の生の値を読む（LibRaw の unprocessed_raw との照合に使う）。
RawSensorData read_cr2_sensor_data(const std::string& path);

// EXIF形式の日時 "YYYY:MM:DD HH:MM:SS" を .NET ticks（0001-01-01 からの100ns単位）にする。
// subsec は SubSecTime の数字列（"14" なら0.14秒）、offset_minutes は UTC からのずれ
// （日本なら +540）。読めなければ 0 を返す。
std::int64_t exif_datetime_to_ticks(const std::string& datetime, const std::string& subsec,
                                    int offset_minutes);

}  // namespace stackcore
