#pragma once

#include <functional>
#include <string>

#include "stackcore/frame_buffer.hpp"
#include "stackcore/ser_decoder.hpp"

namespace stackcore {

class VideoSource;

// ダーク・フラット補正（キャリブレーション）。
//
// 補正は**デバイヤー前の生のCFA画素**に掛ける。デバイヤー後に引くと、
// 補間で隣の色の画素が混ざったあとの値からダークを引くことになり、
// 熱かぶりやホコリの影が色付きのまま残る。
//
//   補正後 = (生の値 - マスターダーク) / 正規化フラット
//
// マスターは動画（または静止画連番）の全フレームを、フレーム番号順に
// float64で平均して作る。順序と精度を固定するので、同じ入力なら常に同じマスターになる。
struct CalibrationFrames {
    FrameBuffer dark;  // 空なら補正しない
    FrameBuffer flat;  // 平均が1になるよう正規化済み。空なら補正しない
    // 解析結果の使い回しを判断するための識別子（ファイルのパスとサイズなど）。
    // 補正を変えたら品質評価からやり直す必要があるので、GUIはこれを指紋に入れる。
    std::string identity;

    bool empty() const { return dark.empty() && flat.empty(); }
};

// false を返すと中断する（ProgressFn と同じ規約。Cancelled を投げる）。
using CalibrationProgressFn = std::function<bool(int done, int total)>;

// 全フレームの平均（デバイヤーしない生の画素）。
FrameBuffer build_master_frame(const VideoSource& source,
                               const CalibrationProgressFn& progress = CalibrationProgressFn());

// 平均が1になるようフラットを正規化する。
// Bayerの場合は2×2の位相（R・G・G・Bの位置）ごとに正規化する。全体で割ると
// 色ごとの感度差まで「補正」してしまい、色のバランスが崩れるため。
// 極端に暗い画素（平均の1%未満）は補正不能として1に置き換える。
FrameBuffer normalize_flat(const FrameBuffer& master_flat, SerColorId pattern);

// 補正を掛ける。寸法・チャンネル数が合わなければ std::runtime_error を投げる。
// 結果は 0..1 に切り詰める。
void apply_calibration(FrameBuffer& frame, const CalibrationFrames& calibration);

}  // namespace stackcore
