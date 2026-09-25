#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace stackcore {

class VideoSource;

// 書き出す画像に添える情報（TIFFタグ・FITSヘッダ・PNGのtEXt）。
//
// **現在時刻を入れてはいけない。** 同じ入力・同じ設定なら出力はバイト単位で
// 一致する、という要件（仕様書 §7.1）が崩れる。入れてよいのは入力から
// 決まる値（撮影時刻・フレーム数・設定）と、ソフトの版だけである。
struct ImageMetadata {
    std::string software;     // 例 "LunaStack 0.3.0"
    std::string description;  // 処理条件の要約（1行）
    std::string object;       // 対象名（任意）
    // 撮影時刻（UTC、ISO 8601 "2021-07-08T10:50:25.300"）。分からなければ空。
    std::string date_obs;
    int frames_combined = 0;  // 加算したフレーム数（FITSのNCOMBINE）
    // 処理の履歴（FITSのHISTORY、TIFF/PNGでは説明に続けて書く）。
    std::vector<std::string> history;

    bool empty() const {
        return software.empty() && description.empty() && object.empty() &&
               date_obs.empty() && frames_combined == 0 && history.empty();
    }
};

// SERのタイムスタンプ（.NET ticks、0001-01-01 00:00:00 UTC からの100ns単位）を
// ISO 8601 の文字列にする（ミリ秒まで）。
std::string ticks_to_iso8601(std::int64_t ticks);

// TIFFのDateTimeタグの書式 "YYYY:MM:DD HH:MM:SS"。
std::string ticks_to_tiff_datetime(std::int64_t ticks);

// WinJUPOSのファイル名に使う時刻 "YYYY-MM-DD-HHMM_T"（T は分の10分の1）。
// WinJUPOSは画像の中央時刻をファイル名から読むため、自転の補正（derotation）に使える。
std::string ticks_to_winjupos(std::int64_t ticks);

// 指定したフレーム（ソースでの番号）の撮影時刻の中央（最初と最後の中点）。
// タイムスタンプが無い、または全て0なら false。
bool mid_timestamp(const VideoSource& source, const std::vector<int>& indices,
                   std::int64_t& ticks);

}  // namespace stackcore
