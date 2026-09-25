#pragma once

// MainWindowController の内部共有ヘッダ。
//
// 画面が大きくなったため、実装を工程ごとのカテゴリ（+Layout / +Settings / +Jobs /
// +Preview / +Finishing / +SelfCheck）に分けている。インスタンス変数と
// カテゴリ間で呼び合うメソッドはすべてここに置く。**実装ファイル以外から読まないこと。**

#import "MainWindowController.h"

#import "Localization.h"
#import "LSAppSupport.h"
#import "Presets.h"
#import "QueueItem.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "stackcore/map_pipeline.hpp"
#include "stackcore/video_source.hpp"
#include "stackcore/wavelet.hpp"

@interface MainWindowController () {
    // 自己検証用の完了通知（@synthesize onRunFinished の実体）。
    void (^_onRunFinished)(void);

    // --- 左ペイン ---
    NSTableView* _queueTable;
    QualityGraphView* _graph;
    NSSegmentedControl* _graphMode;
    NSButton* _clearButton;

    // --- 中央 ---
    PreviewView* _preview;
    NSSegmentedControl* _zoomControl;
    NSButton* _apShowCheck;
    NSButton* _apHeatCheck;
    NSButton* _apEditCheck;
    NSTextField* _apCountLabel;
    NSStackView* _bannerBar;
    NSTextField* _bannerLabel;
    NSButton* _bannerLittleButton;
    NSButton* _bannerBigButton;
    NSButton* _bannerDepthButton;
    NSButton* _bannerCloseButton;

    // --- 右Inspector ---
    NSSegmentedControl* _inspectorTab;   // 工程タブ（解析・スタック ⇄ 仕上げ・書き出し）
    NSSegmentedControl* _modeSegment;
    NSPopUpButton* _methodPopup;
    NSPopUpButton* _endianPopup;
    NSPopUpButton* _depthPopup;
    NSSegmentedControl* _viewModeSegment;
    NSSlider* _frameSlider;
    NSPopUpButton* _apSizePopup;
    NSSlider* _searchRadiusSlider;
    NSTextField* _searchRadiusValue;
    NSSlider* _topSlider;
    NSTextField* _topValue;
    NSButton* _refineCheck;

    NSSlider* _apTopSlider;
    NSTextField* _apTopValue;
    NSTextField* _apTopCaption;
    NSSegmentedControl* _selectionModeSegment;
    NSPopUpButton* _qualityMetricPopup;

    NSButton* _lowMemoryCheck;
    NSButton* _normalizeCheck;
    NSPopUpButton* _stackModePopup;

    NSSegmentedControl* _drizzleSegment;
    NSSlider* _pixfracSlider;
    NSTextField* _pixfracValue;
    NSTextField* _drizzleEstimate;

    NSSlider* _sharpenSliders[kWaveletLayers];
    NSTextField* _sharpenValues[kWaveletLayers];
    NSSlider* _denoiseSliders[kWaveletLayers];
    NSTextField* _denoiseValues[kWaveletLayers];
    NSButton* _waveletPreviewCheck;
    NSButton* _stretchCheck;

    NSPopUpButton* _formatPopup;
    NSTextField* _namePreview;
    NSPopUpButton* _presetPopup;

    // --- 下部 ---
    NSTextField* _statusLabel;
    NSProgressIndicator* _progress;
    NSButton* _qualityButton;
    NSButton* _alignButton;
    NSButton* _stackButton;
    NSButton* _cancelButton;
    NSButton* _saveButton;

    // セクションの開閉。key → その節に属するビュー。
    NSMutableDictionary* _sections;
    // key → 見出しボタン。タブ切替でセクションごと隠すために持つ。
    NSMutableDictionary* _sectionHeaders;

    // --- 状態 ---
    NSMutableArray* _items;         // QueueItem
    NSInteger _currentIndex;

    std::string _inputPath;
    int _sourceChannels;
    int _sourceFrames;
    BOOL _byteOrderSuspect;
    BOOL _looksLikeShallowDepth;  // 16bitコンテナに12bitが入っている疑い
    BOOL _bannerDismissed;
    int _rejectedFrames;
    int _sourceWidth;
    int _sourceHeight;
    std::shared_ptr<stackcore::GlobalStageReport> _qualityStage;
    std::shared_ptr<stackcore::GlobalStageReport> _globalStage;
    NSString* _qualitySignature;
    NSString* _globalSignature;
    std::shared_ptr<stackcore::AnalysisData> _analysis;
    NSString* _analysisSignature;   // _analysis を作ったときの設定
    // 手動配置。**「使うかどうか」と「中身」は別に持つ。**
    // 空リストを自動配置の合図にすると、「すべて消去」が効かなくなる。
    BOOL _manualPointsActive;
    std::vector<stackcore::AlignmentPoint> _manualPoints;
    std::shared_ptr<stackcore::FrameBuffer> _stacked;
    std::shared_ptr<stackcore::FrameBuffer> _displayed;
    std::shared_ptr<stackcore::FrameBuffer> _referenceImage;
    std::shared_ptr<stackcore::WaveletSharpener> _wavelet;

    std::atomic<bool>* _cancelFlag;
    BOOL _running;
    int _frameLimit;
    BOOL _selectionUsesCount;
    double _apTopPercentSetting;
    int _apTopCountSetting;

    // 残り時間の推定。フェーズが変わったら測り直す。
    NSString* _etaStage;
    NSTimeInterval _etaStart;
}
@end

// カテゴリをまたいで呼ぶメソッド。宣言をクラス拡張ではなくカテゴリに置くのは、
// 本体（MainWindowController.mm）に実装が無いという警告を出さないため。
@interface MainWindowController (LSInternal)

- (instancetype)init;
- (void)dealloc;
- (void)buildInterface;
- (NSView*)buildLeftPane;
- (NSView*)buildCenterPane;
- (NSView*)buildRightPane;
- (NSInteger)tabIndexForSectionKey:(NSString*)key;
- (void)inspectorTabChanged:(id)sender;
- (void)updateInspectorVisibility;
- (void)beginSection:(NSString*)title key:(NSString*)key inBox:(NSStackView*)box;
- (void)addToSection:(NSString*)key view:(NSView*)view box:(NSStackView*)box;
- (BOOL)sectionOpen:(NSString*)key;
- (void)updateSectionHeader:(NSButton*)header key:(NSString*)key;
- (void)toggleSection:(id)sender;
- (void)buildAlignmentSection:(NSStackView*)box;
- (void)buildQualitySection:(NSStackView*)box;
- (void)buildStackSection:(NSStackView*)box;
- (void)buildDrizzleSection:(NSStackView*)box;
- (void)buildWaveletSection:(NSStackView*)box;
- (void)buildExportSection:(NSStackView*)box;
- (NSView*)buildStatusBar;
- (NSTextField*)sectionTitle:(NSString*)text;
- (NSButton*)buttonWithTitle:(NSString*)title action:(SEL)action;
- (NSButton*)checkboxWithTitle:(NSString*)title state:(BOOL)on;
- (NSSlider*)sliderMin:(double)lo max:(double)hi value:(double)v action:(SEL)action;
- (NSView*)row:(NSView*)main trailing:(NSView*)trailing;
- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView;
- (NSView*)tableView:(NSTableView*)tableView
    viewForTableColumn:(NSTableColumn*)column
                   row:(NSInteger)row;
- (void)tableViewSelectionDidChange:(NSNotification*)notification;
- (NSDragOperation)tableView:(NSTableView*)tableView
                validateDrop:(id<NSDraggingInfo>)info
                 proposedRow:(NSInteger)row
       proposedDropOperation:(NSTableViewDropOperation)op;
- (BOOL)tableView:(NSTableView*)tableView
       acceptDrop:(id<NSDraggingInfo>)info
              row:(NSInteger)row
    dropOperation:(NSTableViewDropOperation)op;
- (void)addPathsToQueue:(NSArray*)paths;
- (void)fillHeaderInfo:(QueueItem*)item;
- (void)openFileAtPath:(NSString*)path;
- (void)selectQueueIndex:(NSInteger)index;
- (void)removeSelectedFromQueue:(id)sender;
- (void)resetWorkspaceForNewProcessing;
- (void)clearWorkspace:(id)sender;
- (void)clearForTesting;
- (NSInteger)contextQueueRow;
- (void)menuWillOpen:(NSMenu*)menu;
- (BOOL)validateMenuItem:(NSMenuItem*)menuItem;
- (void)revealQueueItemInFinder:(id)sender;
- (void)openDocument:(id)sender;
- (stackcore::OpenOptions)currentOpenOptions;
- (void)inputInterpretationChanged:(id)sender;
- (stackcore::MapStackSettings)currentSettings;
- (NSString*)qualitySignature;
- (NSString*)analysisSignature;
- (BOOL)analysisUsable;
- (BOOL)qualityUsable;
- (BOOL)globalUsable;
- (BOOL)alignmentUsable;
- (void)analysisSettingChanged:(id)sender;
- (void)topChanged:(id)sender;
- (void)apTopChanged:(id)sender;
- (void)selectionModeChanged:(id)sender;
- (void)refreshSelectionControl;
- (void)searchRadiusChanged:(id)sender;
- (void)drizzleChanged:(id)sender;
- (void)updateDrizzleEstimate;
- (void)graphModeChanged:(id)sender;
- (void)qualityGraphView:(QualityGraphView*)view didChangeCutPercent:(double)percent;
- (void)zoomChanged:(id)sender;
- (void)apDisplayChanged:(id)sender;
- (void)updateControlsEnabled;
- (void)startRun;
- (void)startAnalyzeOnly;
- (void)startAlignmentOnly;
- (void)startStackOnly;
- (void)setFrameLimit:(int)limit;
- (void)analyze:(id)sender;
- (void)align:(id)sender;
- (void)run:(id)sender;
- (void)cancel:(id)sender;
- (void)beginJobStage:(JobStage)stage;
- (void)finishJob:(const JobResult&)result
 qualitySignature:(NSString*)qualitySignature
alignmentSignature:(NSString*)alignmentSignature;
- (void)rebuildReferenceImage;
- (void)showFrames:(const std::vector<stackcore::FrameInfo>&)frames;
- (void)updateBanner;
- (void)useLittleEndianFromBanner:(id)sender;
- (void)useBigEndianFromBanner:(id)sender;
- (void)use12BitFromBanner:(id)sender;
- (void)dismissBanner:(id)sender;
- (void)showSourceFrame:(int)index;
- (void)frameSliderChanged:(id)sender;
- (void)viewModeChanged:(id)sender;
- (void)updateApOverlay;
- (void)resetEta;
- (void)reportStage:(NSString*)stage done:(int)done total:(int)total;
- (NSString*)formatSeconds:(double)seconds;
- (void)notifyDone:(NSString*)text;
- (NSString*)inputPathString;
- (NSString*)sidecarPath;
- (NSString*)sidecarSettingsPath;
- (void)saveSidecarForCurrent;
- (void)loadSidecarForCurrent;
- (NSDictionary*)settingsDictionary;
- (void)applySettingsDictionary:(NSDictionary*)d;
- (void)applySettingsDictionary:(NSDictionary*)d includePostProcessing:(BOOL)includePost;
- (void)reloadPresets;
- (void)presetSelected:(id)sender;
- (void)savePreset:(id)sender;
- (BOOL)selfCheckApHitTest;
- (BOOL)selfCheckEditingAndPresets;
- (void)previewView:(PreviewView*)view didAddApAtX:(int)x y:(int)y;
- (void)previewView:(PreviewView*)view didDeleteApAtIndex:(NSInteger)index;
- (void)ensureManualPointsInitialized;
- (void)applyManualPoints;
- (void)resetApPlacement:(id)sender;
- (void)clearApPlacement:(id)sender;
- (void)stretchToggled:(id)sender;
- (void)waveletChanged:(id)sender;
- (void)waveletPreviewChanged:(id)sender;
- (void)resetWavelet:(id)sender;
- (void)applyWavelet;
- (void)setDrizzleIndexForTesting:(int)index;
- (void)setZoomIndexForTesting:(int)index;
- (void)setApHeatmapForTesting:(BOOL)on;
- (void)setSharpenForTesting:(double)value denoise:(double)denoise;
- (void)setWaveletPreviewForTesting:(BOOL)on;
- (OutputFormat)currentOutputFormat;
- (NSString*)outputNameForPath:(NSString*)path apSize:(int)apSize;
- (void)formatChanged:(id)sender;
- (void)updateNamePreview;
- (void)save:(id)sender;
- (void)showError:(NSString*)message title:(NSString*)title;
- (BOOL)windowShouldClose:(NSWindow*)sender;

@end

// デリゲートの採用は、そのメソッドを実装するカテゴリで宣言する。
@interface MainWindowController (Queue) <NSTableViewDataSource, NSTableViewDelegate,
                                         NSMenuDelegate, NSMenuItemValidation>
@end

@interface MainWindowController (Settings) <QualityGraphViewDelegate>
@end

@interface MainWindowController (Preview) <PreviewViewDelegate>
@end
