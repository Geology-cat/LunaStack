#pragma once

#include <memory>

#include "stackcore/frame_buffer.hpp"

namespace stackcore {

struct MatchResult {
    // テンプレートをこれだけ動かすと探索画像に重なる、という変位。
    // サブピクセル推定済み。
    double dx = 0.0;
    double dy = 0.0;
    // 整数ピークの位置（診断用）。
    int peak_dx = 0;
    int peak_dy = 0;
    // ピークのZNCC値（-1..1）。信頼度として使う。
    double score = 0.0;
    // 変位ゼロ（＝グローバル補正だけ）のときのZNCC。
    //
    // ピークの絶対値だけでは、模様が一方向にしかない領域（惑星の縞など）を
    // 見抜けない。そこでは相関面が尾根状になり、ほとんど改善しないまま
    // ピークが探索範囲の縁まで滑る。実測では ZNCC(0,0)=0.978 に対して
    // 16px先の ZNCC=0.986 でしかなく、それでもピークはそこへ行った。
    // 「動かした甲斐がどれだけあったか」を見るための値。
    double score_at_zero = 0.0;
    // ピークが探索範囲の縁に張り付いた。真の変位は範囲外の可能性が高く、
    // サブピクセル推定も片側しか使えないため信用できない（仕様書 §4.6）。
    bool at_search_limit = false;
};

// ZNCC（ゼロ平均正規化相互相関）によるテンプレートマッチング。
//
// 分子は FFT による相互相関、分母は積分画像による窓ごとの平均・分散で求める
// （Lewis 1995 の高速NCC。実装計画書 §4.1）。
// 素朴に候補変位ごとへ空間領域で計算すると、AP64px・探索±16で
// 1回あたり 33x33x4096 ≈ 450万回の積和になり、フレーム数を掛けると破綻する。
//
// ZNCCを使うのは照度変動に強いため（仕様書 §4.6）。薄雲やシーイングによる
// 明るさの変化があっても、平均を引き分散で割るので相関値がぶれない。
class ZnccMatcher {
public:
    // template_size: テンプレート（AP）の一辺
    // search_radius: 探索半径（±この値の範囲を探す）
    ZnccMatcher(int template_size, int search_radius);
    ~ZnccMatcher();

    ZnccMatcher(const ZnccMatcher&) = delete;
    ZnccMatcher& operator=(const ZnccMatcher&) = delete;

    int template_size() const noexcept;
    int search_radius() const noexcept;
    int padded_size() const noexcept;

    // テンプレートを設定する。以降の match() はこれを探す。
    // ゼロ平均・単位ノルム化はここで済ませる（フレームごとにやり直さない）。
    // 分散が0（一様な領域）の場合は false を返し、match() は使えない。
    bool set_template(const float* tmpl, std::size_t stride);

    // 探索画像の中からテンプレートを探す。
    // search はテンプレートの周囲に search_radius ぶんの余白を付けた
    // (template_size + 2*search_radius)^2 の領域。
    MatchResult match(const float* search, std::size_t stride);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 3x3の相関値から2次曲面をあてはめてサブピクセル位置を求める（仕様書 §4.6）。
// values は [dy+1][dx+1] の並び（中心が values[1][1]）。
// 極値が3x3の外に出る場合は補正を ±1 に丸める（外挿は信用できない）。
void quadratic_subpixel(const double values[3][3], double& out_dx, double& out_dy);

}  // namespace stackcore
