#pragma once

#include <cstdint>
#include <vector>

#include "stackcore/frame_buffer.hpp"

namespace stackcore {

enum class StackMode {
    Mean,
    QualityWeighted,
    SigmaClip,
};

struct WindowedStackStats {
    std::size_t uncovered_pixels = 0;  // どのAPからも寄与を受けなかった画素
    std::size_t weak_pixels = 0;       // 重みが小さすぎて代替で埋めた画素
    std::size_t fallback_blended_pixels = 0;  // AP外周で代替画像と滑らかに混ぜた画素
    double min_weight = 0.0;
    double max_weight = 0.0;
    double max_value = 0.0;
    std::size_t clipped = 0;
    int ap_count = 0;
    long long contributions = 0;  // AP×フレームの加算回数の合計
};

// オーバーラップ窓合成スタック（仕様書 §4.8）。本アプリの品質の核。
//
//   S(x,y) = Σ_AP Σ_frame w(r) * I_frame(切り出し)
//   W(x,y) = Σ_AP Σ_frame w(r)
//   L(x,y) = S / W
//   fallback があれば 出力 = clamp(W, 0, 1) * L + (1 - clamp(W, 0, 1)) * fallback
//   fallback がなければ 出力 = L（未被覆は0）
//
// 窓はHann窓（AP中心で1、縁で0）。50%オーバーラップ格子との組で
// 重みの合計がほぼ一様になる。自動配置でAPが疎になる外周は、窓の
// 累積重みを混合率として参照画像へ滑らかにつなぎ、矩形の継ぎ目を防ぐ。
//
// 数値の持ち方（実装計画書 §6.7 の測定にもとづく）:
//   * AP内でフレームを足し込む局所バッファは **float64**。
//     加算の大半はここで起きる。APは64x64程度なので数十KBしか増えない。
//   * 全体の S / W は **float32**。1画素あたりAP4枚ぶんの4回しか加算されない。
//   素朴なfloat32逐次加算は、値0.9をN=30000回足すと16bit出力で16.5階調ずれる。
//   一方この2段階なら誤差はNによらず1LSBの1/600に収まる。
//   float64の全体バッファは4K×3Drizzleで2.39GBとなり、
//   2GBまで引き下げ可能なワーキングセット上限（仕様書 §7.3）を単独で超える。
class WindowedStacker {
public:
    // scale は出力グリッドの倍率（Drizzle。1.0で無効）、pixfrac は入力画素を
    // 縮めてから落とす割合（仕様書 §4.9）。
    //
    // **倍率1.0のときは従来どおりLanczos3で切り出す。**
    // Drizzleの投影経路に一本化しないのは、等倍でわざわざ面積投影する意味がなく、
    // M2までの出力がビット単位で変わってしまうため。
    WindowedStacker(int width, int height, int channels, int ap_size, double scale = 1.0,
                    double pixfrac = 0.9, StackMode mode = StackMode::Mean,
                    double sigma_threshold = 2.0);

    int out_width() const noexcept { return out_width_; }
    int out_height() const noexcept { return out_height_; }

    // ---- AP ごとの足し込みを独立して持つ口（フレームを1回だけ読む窓合成用） ----
    //
    // AP の足し込み（AP内の float64 バッファ）を AP ごとの Ap に持たせる。別々の Ap への
    // accumulate は同時に呼んでよい（互いに干渉しない）。これで「フレームを1枚読んで、
    // そのフレームを選んだ AP すべてへ足す」順に処理でき、AP ごとにフレームを読み直さずに済む。
    //
    // **1つの Ap の中ではフレーム番号の昇順に accumulate し、commit は AP 番号の昇順に
    // 呼ぶこと。** そうすれば begin_ap/add_frame/end_ap と加算の順序が同じになり、
    // 出力はバイト単位で一致する。
    struct Ap {
        int cx = 0, cy = 0;
        int ox = 0, oy = 0;  // 出力グリッド上でのAP左上
        int frames = 0;
        double weight_sum = 0.0;
        std::vector<double> accum;     // AP内のフレーム加算（float64）
        std::vector<double> coverage;  // Drizzle・σクリップの被覆量
        std::vector<float> samples;    // σクリップのときだけ、各フレームを保持
        // 作業領域（AP ごとに持つので、並列に足してもぶつからない）
        std::vector<float> patch;
        std::vector<double> frame_accum, frame_coverage;
    };
    void start_ap(Ap& ap, int center_x, int center_y) const;
    void accumulate(Ap& ap, const FrameBuffer& frame, double dx, double dy, double gain,
                    double sample_weight = 1.0) const;
    // AP の加算を確定して全体バッファへ足し込み、Ap のメモリを手放す。
    void commit_ap(Ap& ap);
    // AP 1つが（フレームを frames 枚足したとき）使うおよそのバイト数。組分けの目安。
    std::size_t ap_bytes(int frames) const;

    // AP1点ぶんの加算を開始する。
    void begin_ap(int center_x, int center_y);

    // 現在のAPに1フレームぶんを加算する。
    // frame から (center - half + dx, center - half + dy) を左上として
    // ap_size 角をLanczos3で切り出す。gain は輝度正規化の係数。
    //
    // **呼び出し順はAP番号昇順・その中でフレーム番号昇順に固定すること**
    // （決定論性の要件。実装計画書 §4.2）。
    void add_frame(const FrameBuffer& frame, double dx, double dy, double gain,
                   double sample_weight = 1.0);

    // 現在のAPの加算を確定し、全体バッファへ足し込む。
    void end_ap();

    // fallback は、どのAPからも寄与を受けなかった画素を埋める画像
    // （グローバルアライメントのみで作った参照画像を渡す想定）。
    // APの累積Hann重みが1未満の画素では、重みを混合率として局所スタックから
    // fallback へ滑らかにつなぐ。fallback が空なら従来どおり S/W を返す。
    // 空なら0で埋める。
    // Drizzleで出力が拡大されている場合は、fallback をLanczos3で拡大して使う。
    void finish(FrameBuffer& out, WindowedStackStats& stats,
                const FrameBuffer* fallback) const;

private:
    void add_frame_lanczos(Ap& ap, const FrameBuffer& frame, double dx, double dy, double gain,
                           double sample_weight) const;
    void add_frame_drizzle(Ap& ap, const FrameBuffer& frame, double dx, double dy, double gain,
                           double sample_weight) const;
    void prepare_sigma_ap(Ap& ap) const;

    // 入力サイズは持たない。Drizzle対応で出力グリッドを別に持つようにした際、
    // 参照するのは出力側だけになった。
    int channels_ = 0, ap_size_ = 0;
    int out_width_ = 0, out_height_ = 0;
    int ap_out_ = 0;      // 出力グリッド上でのAPの一辺
    double scale_ = 1.0;
    double pixfrac_ = 0.9;
    bool drizzle_ = false;
    StackMode mode_ = StackMode::Mean;
    double sigma_threshold_ = 2.0;

    bool in_ap_ = false;
    Ap current_;                      // begin_ap/add_frame/end_ap で使う AP
    std::vector<float> window_;       // ap_out_ x ap_out_ のHann窓
    // σクリップ時の Ap::samples の並びは frame → channel → pixel。Drizzleの未被覆値はNaN。
    std::vector<float> sum_;         // 全体 S（float32）
    std::vector<float> weight_;      // 全体 W（float32）
    int ap_count_ = 0;
    long long contributions_ = 0;
};

}  // namespace stackcore
