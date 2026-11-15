#pragma once

#include <string>
#include <vector>

#include "stackcore/frame_buffer.hpp"
#include "stackcore/global_stage.hpp"

namespace stackcore {

// 月面などのモザイク合成。
//
// スタック済みの画像（パネル）を複数受け取り、重なりから互いの位置を求めて
// 1枚につなぐ。パネルの並び順・配置（何行何列か）は指定しなくてよい。
//
// 流れ:
//   1. 粗い位置合わせ   : 縮小した画像どうしで、重なりだけを見た正規化相互相関
//                         （マスク付きNCC）を全変位について求める。重なりが一部しか
//                         ないパネルどうしでも、重なった部分の一致だけで判定できる。
//   2. 細かい位置合わせ : 重なりの中に小さな領域を敷き詰め、ZNCC でサブピクセルの
//                         対応点を取る。対応点どうしの整合で、粗い候補の当否を確かめる。
//   3. 全体の調整       : すべての対応点を同時に満たす配置を最小二乗で解く。
//                         経緯台の視野回転のような小さな回転・倍率差も補正する
//                         （不要なら平行移動だけにする）。
//   4. 明るさ合わせ     : 重なりの平均輝度が合うように、パネルごとの倍率を解く。
//                         空が写っていれば、その明るさ（カメラの下駄）を基準にして
//                         倍率を掛け、空の明るさはパネル間でそろえる。
//   5. 合成             : 低い周波数は広くなだらかに、高い周波数は継ぎ目の近くだけで
//                         混ぜる（2帯域ブレンド）。明るさの段差は広くぼかして消し、
//                         細部は二重像にならないよう片方のパネルから取る。
struct MosaicSettings {
    // パネル間の明るさ（露出・透明度の差）をそろえる。
    bool match_brightness = true;
    // 小さな回転・倍率差も補正する。false なら平行移動だけで合わせる。
    bool allow_rotation = true;
};

// パネル1枚の配置。パネルの画素 (x, y) は、モザイクの
//   X = a x - b y + tx,  Y = b x + a y + ty
// に置かれる（回転なしなら a = 1, b = 0）。
struct MosaicPlacement {
    bool placed = false;
    double a = 1.0;
    double b = 0.0;
    double tx = 0.0;
    double ty = 0.0;
    double gain = 1.0;       // 明るさ合わせの倍率
    int links = 0;           // 位置が合った相手のパネル数
};

struct MosaicReport {
    int width = 0;
    int height = 0;
    int placed = 0;                         // 配置できたパネル数
    bool used_rotation = false;             // 回転・倍率の補正を使った
    double max_rotation_degrees = 0.0;      // 補正した回転の最大（度）
    double residual_rms = 0.0;              // 対応点の残差（画素、RMS）
    int correspondences = 0;                // 配置に使った対応点の数
    double background = 0.0;                // 明るさ合わせの基準にした背景の輝度（無ければ0）
    std::vector<MosaicPlacement> panels;    // 入力と同じ順
};

// パネルをつないだ画像を返す。チャンネル数は、カラーのパネルが1枚でもあれば3。
//
// 重なりが見つからず、どのパネルともつながらなかったパネルは配置しない
// （report.panels[i].placed が false）。1組もつながらなければ std::runtime_error。
// progress が false を返したら Cancelled を投げる。
FrameBuffer build_mosaic(const std::vector<const FrameBuffer*>& panels,
                         const MosaicSettings& settings, const ProgressFn& progress,
                         MosaicReport& report);

}  // namespace stackcore
