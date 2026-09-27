#pragma once

#import <Cocoa/Cocoa.h>

#include <cstdint>
#include <vector>

@class LevelsView;

@protocol LevelsViewDelegate <NSObject>
// 三角をドラッグしている間も続けて呼ぶ。
- (void)levelsViewDidChange:(LevelsView*)view;
@end

// レベル補正の入力レベル（Photoshop の「レベル補正」と同じ見た目・操作）。
//
// 上にレベル補正に入る画像のヒストグラム、下に黒・中間・白の3つの三角を置く。
//   * 黒・白の三角: 0..1 の位置（画面では 0〜255）
//   * 中間の三角 : 黒〜白の間の割合 t = 0.5^gamma の位置。左へ動かすと gamma > 1（明るくなる）
//   * 黒・白を動かしても、中間の三角は黒〜白の間の割合を保つ（gamma は変わらない）
@interface LevelsView : NSView

@property(nonatomic) double black;  // 0..1
@property(nonatomic) double white;  // 0..1（black より大きい）
@property(nonatomic) double gamma;  // 0.10〜9.99
@property(nonatomic) BOOL enabled;
@property(nonatomic, assign) id<LevelsViewDelegate> delegate;

// ヒストグラム（段数は任意。空なら何も描かない）。
- (void)setHistogram:(const std::vector<std::uint32_t>&)counts;

@end

// 中間の三角の割合と gamma の変換（どちらも LevelsView と数値欄で使う）。
double LSLevelsGammaFromMidpoint(double t);
double LSLevelsMidpointFromGamma(double gamma);
constexpr double kLevelsGammaMin = 0.10;
constexpr double kLevelsGammaMax = 9.99;
