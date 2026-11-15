#pragma once

#import <Cocoa/Cocoa.h>

// 採用率の目安のグラフ。横軸は採用率（%）、縦軸は再現性の点数。
// 目安の点を強調し、いまの採用率を縦線で示す。
@interface AdoptionGraphView : NSView

// percents・scores は同じ長さ（採用率の昇順）。suggested は目安の採用率。
- (void)setPercents:(const double*)percents
             scores:(const double*)scores
              count:(int)count
          suggested:(double)suggested;
- (void)clearData;
- (BOOL)hasData;

// いまの採用率（%）。負なら描かない。
@property(nonatomic) double currentPercent;

@end
