#pragma once

#import <Cocoa/Cocoa.h>

#import "PreviewView.h"
#import "QualityGraphView.h"

// 3ペイン構成（UI設計書 §2）。
//   左   : 入力キュー ＋ 品質グラフ
//   中央 : プレビュー（APオーバーレイ・編集）
//   右   : Inspector（Alignment / Quality / Stack / Drizzle / Wavelet / Export）
//   下   : ステータスバー（フェーズ名・進捗・残り時間・中断）
@interface MainWindowController
    : NSWindowController <NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate,
                          NSMenuDelegate, NSMenuItemValidation, QualityGraphViewDelegate,
                          PreviewViewDelegate>

- (instancetype)init;

// キューに追加して、いちばん最後に足したものを選択する。
- (void)openFileAtPath:(NSString*)path;
- (void)addPathsToQueue:(NSArray*)paths;

// ---- 自己検証用 ----
// GUIを人が操作しなくても「開く→品質評価→アライメント→スタック」の
// 経路を通せる。通常のGUIでは各工程で必ず停止する。
@property(nonatomic, copy) void (^onRunFinished)(void);
- (void)startRun;         // 自己検証用の一括処理
- (void)startAnalyzeOnly; // 品質評価だけ
- (void)startAlignmentOnly;
- (void)startStackOnly;
- (void)startBatch;
- (void)setFrameLimit:(int)limit;
- (void)setSharpenForTesting:(double)value denoise:(double)denoise;
- (void)setWaveletPreviewForTesting:(BOOL)on;
- (void)setBatchOutputDirectory:(NSString*)path;
// APの当たり判定が描画とずれていないかを確かめる。
- (BOOL)selfCheckApHitTest;
// AP編集とプリセットの往復が壊れていないかを確かめる。
- (BOOL)selfCheckEditingAndPresets;
- (void)setApHeatmapForTesting:(BOOL)on;
- (void)setDrizzleIndexForTesting:(int)index;
// ズーム（0=全体 / 1=等倍 / 2=2倍 / 3=4倍）。はみ出しの検証用。
- (void)setZoomIndexForTesting:(int)index;

@end
