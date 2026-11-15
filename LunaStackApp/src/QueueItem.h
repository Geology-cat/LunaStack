#pragma once

#import <Cocoa/Cocoa.h>

#include <memory>

#include "stackcore/frame_buffer.hpp"

typedef NS_ENUM(NSInteger, QueueItemState) {
    QueueItemStatePending = 0,           // 未処理
    QueueItemStateQualityEvaluated = 1,  // フレーム品質の評価済み
    QueueItemStateAnalyzed = 2,          // アライメント済み（サイドカーあり）
    QueueItemStateStacked = 3,           // スタック完了（バッチ時は書き出しも完了）
    QueueItemStateError = 4,             // 失敗
    QueueItemStateImage = 5,             // スタック済みの画像（仕上げ・出力だけを行う）
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

// スタック済みの画像（動画ではなく、仕上げ・出力だけを行う項目）。
// 画像ファイル1枚、またはモザイク合成の結果（ファイルは無く、画像をメモリに持つ）。
@property(nonatomic) BOOL isImage;
// モザイク合成の結果なら、つないだパネルの枚数（それ以外は 0）。
@property(nonatomic) int panelCount;
// モザイク合成の結果そのもの（画像ファイルの項目では空。選ぶたびにファイルから読む）。
- (std::shared_ptr<stackcore::FrameBuffer>)image;
- (void)setImage:(std::shared_ptr<stackcore::FrameBuffer>)image;

+ (instancetype)itemWithPath:(NSString*)path;
// 画像ファイル1枚を仕上げ・出力で開く項目。
+ (instancetype)imageItemWithPath:(NSString*)path;
// モザイク合成の結果。path は書き出し名と保存先を決めるための仮のパス（実在しない）。
+ (instancetype)mosaicItemWithPath:(NSString*)path
                             image:(std::shared_ptr<stackcore::FrameBuffer>)image
                        panelCount:(int)panelCount;
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
