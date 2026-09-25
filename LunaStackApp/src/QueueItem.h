#pragma once

#import <Cocoa/Cocoa.h>

typedef NS_ENUM(NSInteger, QueueItemState) {
    QueueItemStatePending = 0,           // 未処理
    QueueItemStateQualityEvaluated = 1,  // フレーム品質の評価済み
    QueueItemStateAnalyzed = 2,          // アライメント済み（サイドカーあり）
    QueueItemStateStacked = 3,           // スタック完了（バッチ時は書き出しも完了）
    QueueItemStateError = 4,             // 失敗
};

// 入力キューの1行（UI設計書 §3.1）。
@interface QueueItem : NSObject

// 動画のパス。静止画連番ではフォルダのパス（一覧指定のときは先頭の画像のフォルダ）。
@property(nonatomic, copy) NSString* path;
// 静止画連番のファイル一覧。空なら動画（またはフォルダ全体の連番）。
@property(nonatomic, copy) NSArray* sequenceFiles;
// 静止画連番として扱うか（フォルダ・一覧のどちらでも YES）。
@property(nonatomic) BOOL isSequence;
@property(nonatomic) QueueItemState state;
// "4,617フレーム · 448×448 · SER v3" のような副題。開いた時点で埋める。
@property(nonatomic, copy) NSString* subtitle;
// エラー時の説明。
@property(nonatomic, copy) NSString* message;

+ (instancetype)itemWithPath:(NSString*)path;
// 静止画連番。files が空ならフォルダ直下のすべての画像を使う。
+ (instancetype)sequenceItemWithDirectory:(NSString*)directory files:(NSArray*)files;

// 一覧に出す名前（連番は「フォルダ名（N枚）」）。
- (NSString*)displayName;
// 同じ入力かどうか（キューの重複を避けるため）。
- (BOOL)isSameInputAs:(QueueItem*)other;

// 状態の表示名と色。表とプレビューで同じ言葉を使うため一箇所にまとめる。
- (NSString*)stateSymbol;
- (NSColor*)stateColor;

@end
