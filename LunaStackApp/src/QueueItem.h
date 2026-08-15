#pragma once

#import <Cocoa/Cocoa.h>

typedef NS_ENUM(NSInteger, QueueItemState) {
    QueueItemStatePending = 0,   // 未処理
    QueueItemStateAnalyzed = 1,  // 解析済み（サイドカーあり）
    QueueItemStateStacked = 2,   // スタック済み（書き出し済み）
    QueueItemStateError = 3,     // 失敗
};

// 入力キューの1行（UI設計書 §3.1）。
@interface QueueItem : NSObject

@property(nonatomic, copy) NSString* path;
@property(nonatomic) QueueItemState state;
// "4,617フレーム · 448×448 · SER v3" のような副題。開いた時点で埋める。
@property(nonatomic, copy) NSString* subtitle;
// エラー時の説明。
@property(nonatomic, copy) NSString* message;

+ (instancetype)itemWithPath:(NSString*)path;

// 状態の表示名と色。表とプレビューで同じ言葉を使うため一箇所にまとめる。
- (NSString*)stateSymbol;
- (NSColor*)stateColor;

@end
