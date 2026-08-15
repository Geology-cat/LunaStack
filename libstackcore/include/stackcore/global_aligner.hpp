#pragma once

#include <memory>

#include "stackcore/frame_buffer.hpp"

namespace stackcore {

// 対象の種類。仕様書 §4.2。
//   Planet: 暗い背景に明るい対象。閾値二値化＋輝度重心で粗位置→位相相関で精密化。
//   Lunar : 視野全面が対象。縮小画像全面の位相相関。
//   Auto  : 参照フレームの輝度分布から選ぶ。
enum class AlignMode { Auto, Planet, Lunar };

const char* to_string(AlignMode mode);

enum class RejectReason {
    None,
    LowCorrelation,     // 参照とまるで似ていない（追跡失敗・曇り・ドロップ）
    ShiftTooLarge,      // 変位が許容量を超えた（視野外へ流れた）
    StructuralOutlier,  // 他のフレームと比べて類似度だけが明らかに外れている
};

const char* to_string(RejectReason reason);

struct GlobalAlignResult {
    int dx = 0;  // このフレームをこれだけ動かすと参照に重なる
    int dy = 0;

    // 推定した変位で重ねたときの、参照フレームとの正規化相互相関（ZNCC、-1..1）。
    // 追跡が成功していれば参照とよく似るので1に近づき、
    // 曇り・ドロップフレーム・追跡失敗では0付近に落ちる。
    double similarity = 0.0;

    // 相関面のピーク対サイドローブ比。診断用に残している（判定には使わない）。
    double peak_sidelobe_ratio = 0.0;

    bool accepted = false;
    RejectReason reason = RejectReason::None;
};

struct GlobalAlignSettings {
    AlignMode mode = AlignMode::Auto;

    // 参照フレームとの類似度（ZNCC）の下限。これを下回ると追跡失敗として除外する。
    //
    // 相関面の形（ピーク比・ピーク対サイドローブ比）で判定するのは断念した。
    // 暗背景の惑星動画では白色化を弱めざるを得ず、その結果ピークが
    // 幅の広い丘になるため、鋭さを測る指標では良否が分離しない。
    // 実測値（PSR）は実データの良フレーム3.7に対し一様な黒フレーム3.0であり、
    // しきい値を置ける差がなかった。
    // ZNCCは「実際にどれだけ似ているか」を直接測るので、
    // 相関面の形に左右されない。
    double min_similarity = 0.5;

    // 許容する変位の上限（画素）。0で自動（短辺の1/4）。
    int max_shift = 0;
};

// 輝度重心。閾値以上の画素だけを重みとして使う（仕様書 §4.2 の「閾値二値化＋輝度重心」）。
// 対象が見つからない（閾値を超える画素がない）場合は false を返す。
bool luma_centroid(const FrameBuffer& frame, double& cx, double& cy);

// フレーム全体の平均輝度。スタック時の輝度正規化に使う。
double mean_luma(const FrameBuffer& frame);

// 参照フレームの輝度分布から対象の種類を推定する。
// 暗い画素が過半を占めれば「暗背景に浮かぶ惑星」、そうでなければ広視野とみなす。
AlignMode detect_align_mode(const FrameBuffer& reference);

class GlobalAligner {
public:
    GlobalAligner(const FrameBuffer& reference, const GlobalAlignSettings& settings);
    ~GlobalAligner();

    GlobalAligner(const GlobalAligner&) = delete;
    GlobalAligner& operator=(const GlobalAligner&) = delete;

    // Auto を解決したあとの実際のモード。
    AlignMode mode() const noexcept;
    int max_shift() const noexcept;

    GlobalAlignResult align(const FrameBuffer& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace stackcore
