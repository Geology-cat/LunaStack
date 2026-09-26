#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace stackcore {

// ロスレスJPEG（ITU-T T.81 のプロセス14、SOF3。いわゆる LJ92）の自前デコーダ。
//
// カメラのRAW（Canon CR2・DNG の圧縮形式7）の画素はこれで圧縮されている。
// 可逆なので、正しく実装すれば出力は一意に決まる（OSのデコーダに頼らない。仕様書 §8-7）。
//
// 対応: 予測子1〜7、1〜4成分のインターリーブ（各成分の標本化係数は1×1のみ）、
// 精度2〜16bit、行の境目でのリスタートマーカー、点変換（Pt）。
// sRAW/mRAW（YCbCrの間引き）は標本化係数が1×1でないので例外にする。
struct LosslessJpegImage {
    int width = 0;       // SOF3 の幅（1行あたりのMCU数）
    int height = 0;      // SOF3 の高さ
    int components = 0;  // 成分数
    int precision = 0;   // 標本の精度（bit）
    // 行ごとに、MCU順・成分順に並べた標本（1行 width*components 個）。
    std::vector<std::uint16_t> samples;
};

// data は SOI（FF D8）から始まる1つのJPEGストリーム。失敗時は std::runtime_error。
// max_samples が0でなければ、標本数がそれを超える寸法なら展開前に例外にする。
LosslessJpegImage decode_lossless_jpeg(const std::uint8_t* data, std::size_t size,
                                       std::size_t max_samples = 0);

// ヘッダ（SOF3）だけを読む。samples は空のまま。
LosslessJpegImage probe_lossless_jpeg(const std::uint8_t* data, std::size_t size);

}  // namespace stackcore
