#pragma once

#import <Cocoa/Cocoa.h>

@class QualityGraphView;

@protocol QualityGraphViewDelegate <NSObject>
// カットラインがドラッグされた。選択率（上位N%）を渡す。
- (void)qualityGraphView:(QualityGraphView*)view didChangeCutPercent:(double)percent;
@end

// フレーム品質のグラフ（UI設計書 §3.2）。
//
// 横軸はフレーム番号（時系列）と品質順ソートを切り替えられる。
// 時系列表示は、雲の通過やピント変更が「へこみ」として見えるので診断価値が高い。
//
// カットラインは**品質の値**（縦位置）で持ち、選択率へは
// 「その値以上のフレームが何割か」として換算する。
// こうすると時系列表示でも品質順表示でも同じ線が同じ意味を持つ。
@interface QualityGraphView : NSView

// 解析結果を渡す。accepted が false のフレームは除外されたもの。
- (void)setQualities:(const double*)qualities
            accepted:(const unsigned char*)accepted
               count:(int)count;
- (void)clearData;
- (BOOL)hasData;

// 選択率（上位N%）。エンジンの select_top_frames と同じ意味にする。
// すなわち**採用されたフレームのうち**の上位N%である。
@property(nonatomic) double cutPercent;

// NO=フレーム番号順（時系列） / YES=品質降順
@property(nonatomic) BOOL sortedByQuality;

@property(nonatomic, assign) id<QualityGraphViewDelegate> delegate;

@end
