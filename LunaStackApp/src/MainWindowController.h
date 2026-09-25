#pragma once

#import <Cocoa/Cocoa.h>

#import "PreviewView.h"
#import "QualityGraphView.h"

// 3ペイン構成（UI設計書 §2）。
//   左   : 入力キュー ＋ 品質グラフ
//   中央 : プレビュー（APオーバーレイ・編集）
//   右   : Inspector（Alignment / Quality / Stack / Drizzle / Wavelet / Export）
//   下   : ステータスバー（フェーズ名・進捗・残り時間・中断）
//
// 実装は工程ごとのカテゴリに分かれている（MainWindowController_Private.h 参照）。
// デリゲートの採用も、そのメソッドを実装するカテゴリの側で宣言する。
@interface MainWindowController : NSWindowController <NSWindowDelegate>

- (instancetype)init;
@property(nonatomic, copy) void (^onRunFinished)(void);

@end

@interface MainWindowController (LSPublic)

// キューに追加して、いちばん最後に足したものを選択する。
- (void)openFileAtPath:(NSString*)path;
- (void)addPathsToQueue:(NSArray*)paths;

// ---- 自己検証用 ----
// GUIを人が操作しなくても「開く→品質評価→アライメント→スタック」の
// 経路を通せる。通常のGUIでは各工程で必ず停止する。
- (void)startRun;         // 自己検証用の一括処理
- (void)startAnalyzeOnly; // 品質評価だけ
- (void)startAlignmentOnly;
- (void)startStackOnly;
- (void)setFrameLimit:(int)limit;
- (void)setSharpenForTesting:(double)value denoise:(double)denoise;
- (void)setWaveletPreviewForTesting:(BOOL)on;
- (void)clearForTesting;
// APの当たり判定が描画とずれていないかを確かめる。
- (BOOL)selfCheckApHitTest;
// AP編集とプリセットの往復が壊れていないかを確かめる。
- (BOOL)selfCheckEditingAndPresets;
- (void)setApHeatmapForTesting:(BOOL)on;
- (void)setDrizzleIndexForTesting:(int)index;
// ズーム（0=全体 / 1=等倍 / 2=2倍 / 3=4倍）。はみ出しの検証用。
- (void)setZoomIndexForTesting:(int)index;

@end
