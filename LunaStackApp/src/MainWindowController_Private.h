#pragma once

// MainWindowController の内部共有ヘッダ。
//
// 画面が大きくなったため、実装を工程ごとのカテゴリ（+Layout / +Queue / +Settings /
// +Jobs / +Preview / +Finishing / +SelfCheck）に分けている。インスタンス変数と
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

#include "stackcore/calibration.hpp"
#include "stackcore/finishing.hpp"
#include "stackcore/map_pipeline.hpp"
#include "stackcore/video_source.hpp"
#include "stackcore/wavelet.hpp"

@interface MainWindowController () {
    // 自己検証用の完了通知（@synthesize onRunFinished の実体）。
    void (^_onRunFinished)(void);

    // --- 全体の配置（左右ペインの折りたたみ用） ---
    NSView* _leftPane;
    NSView* _rightPane;
    NSLayoutConstraint* _leftWidth;
    NSLayoutConstraint* _leftGap;
    NSLayoutConstraint* _rightWidth;
    NSLayoutConstraint* _rightGap;

    // --- 左ペイン ---
    NSTableView* _queueTable;
    QualityGraphView* _graph;
    NSSegmentedControl* _graphMode;
    NSButton* _clearButton;
    NSTextField* _graphHint;

    // --- 中央 ---
    PreviewView* _preview;
    NSSegmentedControl* _zoomControl;
    NSPopUpButton* _displayMenu;     // 表示の切り替え（プルダウン）
    NSButton* _apEditCheck;
    NSTextField* _apCountLabel;
    NSTextField* _frameInfoLabel;    // フレーム番号・上位何%・品質
    NSTextField* _pixelLabel;        // カーソル位置の画素値
    NSStackView* _bannerBar;
    NSTextField* _bannerLabel;
    NSButton* _bannerLittleButton;
    NSButton* _bannerBigButton;
    NSButton* _bannerDepthButton;
    NSButton* _bannerDetailsButton;
    NSButton* _bannerCloseButton;
    NSSegmentedControl* _viewModeSegment;
    NSSlider* _frameSlider;
    NSSegmentedControl* _frameOrderSegment;  // スライダーの並び（時系列 / 品質順）

    // --- 右Inspector ---
    NSSegmentedControl* _inspectorTab;
    NSPopUpButton* _presetPopup;

    // 品質評価タブ
    NSPopUpButton* _qualityMetricPopup;
    NSPopUpButton* _endianPopup;
    NSPopUpButton* _depthPopup;
    NSPopUpButton* _bayerPopup;
    NSPopUpButton* _debayerPopup;
    NSTextField* _rangeStartField;
    NSTextField* _rangeEndField;
    NSTextField* _darkLabel;
    NSTextField* _flatLabel;
    NSTextField* _outlierKField;
    NSTextField* _minSimilarityField;
    NSTextField* _maxShiftField;

    // アライメントタブ
    NSPopUpButton* _methodPopup;
    NSSegmentedControl* _modeSegment;
    NSPopUpButton* _apSizePopup;
    NSSlider* _searchRadiusSlider;
    NSTextField* _searchRadiusValue;
    NSSlider* _topSlider;
    NSTextField* _topValue;
    NSButton* _refineCheck;
    NSTextField* _minScoreField;
    NSTextField* _apGradientField;
    NSTextField* _apLevelField;
    NSTextField* _alignSummaryLabel;

    // スタックタブ
    NSSlider* _apTopSlider;
    NSTextField* _apTopValue;
    NSTextField* _apTopCaption;
    NSSegmentedControl* _selectionModeSegment;
    NSButton* _lowMemoryCheck;
    NSButton* _normalizeCheck;
    NSButton* _rawCfaCheck;
    NSPopUpButton* _stackModePopup;
    NSTextField* _sigmaField;
    NSSegmentedControl* _drizzleSegment;
    NSSlider* _pixfracSlider;
    NSTextField* _pixfracValue;
    NSTextField* _drizzleEstimate;

    // 仕上げ・出力タブ
    NSButton* _waveletPreviewCheck;
    NSTextField* _channelFields[4];  // R dx, R dy, B dx, B dy
    NSSlider* _sharpenSliders[kWaveletLayers];
    NSTextField* _sharpenValues[kWaveletLayers];
    NSSlider* _denoiseSliders[kWaveletLayers];
    NSTextField* _denoiseValues[kWaveletLayers];
    NSButton* _linkedCheck;
    NSSlider* _linkedSlider;
    NSTextField* _linkedValue;
    NSSlider* _deringSlider;
    NSTextField* _deringValue;
    NSSlider* _gainSliders[3];
    NSTextField* _gainValues[3];
    NSSlider* _saturationSlider;
    NSTextField* _saturationValue;
    NSButton* _toneCheck;
    NSSlider* _blackSlider;
    NSTextField* _blackValue;
    NSSlider* _whiteSlider;
    NSTextField* _whiteValue;
    NSSlider* _gammaSlider;
    NSTextField* _gammaValue;
    NSTextField* _rotationLabel;
    NSButton* _flipHCheck;
    NSButton* _flipVCheck;
    NSButton* _cropCheck;
    NSTextField* _cropMarginField;
    NSTextField* _cropLabel;
    NSPopUpButton* _formatPopup;
    NSPopUpButton* _nameStylePopup;
    NSButton* _metadataCheck;
    NSTextField* _objectField;
    NSTextField* _namePreview;
    NSTextField* _multiPercentField;
    NSButton* _multiExportButton;

    // --- 下部 ---
    NSTextField* _statusLabel;
    NSProgressIndicator* _progress;
    NSButton* _qualityButton;
    NSButton* _alignButton;
    NSButton* _stackButton;
    NSButton* _exportButton;
    NSButton* _cancelButton;
    NSButton* _saveButton;

    // セクションの開閉。key → その節に属するビュー。
    NSMutableDictionary* _sections;
    // key → 見出しボタン。タブ切替でセクションごと隠すために持つ。
    NSMutableDictionary* _sectionHeaders;

    // --- 入力 ---
    NSMutableArray* _items;  // QueueItem
    NSInteger _currentIndex;
    std::string _inputPath;
    std::vector<std::string> _sequenceFiles;  // 静止画連番の一覧（空ならフォルダ全体か動画）
    BOOL _inputIsSequence;
    // プレビュー用に開いたままにしておく入力（メインスレッド専用）。
    // バックグラウンドの処理は、低メモリモードの張り直しと競合しないよう自分で開く。
    std::shared_ptr<stackcore::VideoSource> _previewSource;
    // _previewSource を開いたときの読み方。変わっていなければ開き直さない。
    NSString* _openedInputSignature;
    int _sourceChannels;
    int _sourceFrames;       // 前処理（フレーム範囲）を掛けたあとのフレーム数
    int _sourceTotalFrames;  // ファイル全体のフレーム数
    int _sourceWidth;
    int _sourceHeight;
    BOOL _byteOrderSuspect;
    BOOL _looksLikeShallowDepth;  // 16bitコンテナに12bitが入っている疑い
    BOOL _bannerDismissed;
    int _rejectedFrames;

    // ダーク・フラット補正。
    NSString* _darkPath;
    NSString* _flatPath;
    std::shared_ptr<const stackcore::CalibrationFrames> _calibration;
    BOOL _calibrationDirty;  // 素材を選び直したが、マスターをまだ作っていない

    // --- 各工程の結果 ---
    std::shared_ptr<stackcore::GlobalStageReport> _qualityStage;
    std::shared_ptr<stackcore::GlobalStageReport> _globalStage;
    NSString* _qualitySignature;
    NSString* _globalSignature;
    std::shared_ptr<stackcore::AnalysisData> _analysis;
    NSString* _analysisSignature;  // _analysis を作ったときの設定
    std::shared_ptr<stackcore::MapStackReport> _mapReport;  // アライメントの内訳（表示用）
    std::vector<stackcore::FrameInfo> _frameInfos;           // グラフ・スライダー用
    std::vector<int> _qualityOrder;  // 品質順の並び（採用フレームを品質降順 → 除外フレーム）
    // 手動配置。**「使うかどうか」と「中身」は別に持つ。**
    // 空リストを自動配置の合図にすると、「すべて消去」が効かなくなる。
    BOOL _manualPointsActive;
    std::vector<stackcore::AlignmentPoint> _manualPoints;
    std::shared_ptr<stackcore::FrameBuffer> _referenceImage;

    // --- スタック結果と仕上げ ---
    std::shared_ptr<stackcore::FrameBuffer> _stacked;
    // スタックしたときの設定（書き出し名とAP枠の倍率はこちらに従う。
    // つまみを後から動かしても、画像を作った条件は変わらないため）。
    NSDictionary* _stackedInfo;
    std::vector<int> _stackedFrames;  // 加算に使ったフレーム（撮影時刻の計算用）
    std::shared_ptr<const stackcore::FrameBuffer> _displayed;  // 仕上げ済み
    std::shared_ptr<stackcore::FinishingPipeline> _finishing;
    std::shared_ptr<stackcore::FinishingPipeline> _finishingDraft;  // ドラッグ中の縮小版
    dispatch_queue_t _finishQueue;  // 仕上げの描画を順に行う直列キュー
    long _renderGeneration;         // 最新の描画要求の番号（古い結果を捨てる）
    int _rotationTurns;
    NSRect _cropRect;               // スタック結果の座標。幅0で未指定

    // --- 実行状態 ---
    std::atomic<bool>* _cancelFlag;
    BOOL _running;
    BOOL _terminateAfterCancel;
    BOOL _closeAfterCancel;
    int _frameLimit;
    BOOL _selectionUsesCount;
    double _apTopPercentSetting;
    int _apTopCountSetting;
    BOOL _frameOrderByQuality;
    BOOL _restoringSettings;  // 設定の復元中（保存・無効化の連鎖を止める）

    // 残り時間の推定。フェーズが変わったら測り直す。
    NSString* _etaStage;
    NSTimeInterval _etaStart;
}
@end

// カテゴリをまたいで呼ぶメソッド。宣言をクラス拡張ではなくカテゴリに置くのは、
// 本体（MainWindowController.mm）に実装が無いという警告を出さないため。
@interface MainWindowController (LSInternal)

// ---- 画面の組み立て（+Layout） ----
- (void)buildInterface;
- (NSView*)buildLeftPane;
- (NSView*)buildCenterPane;
- (NSView*)buildRightPane;
- (NSView*)buildStatusBar;
- (void)updateInspectorVisibility;
- (void)inspectorTabChanged:(id)sender;
- (NSTextField*)sectionTitle:(NSString*)text;
- (NSButton*)buttonWithTitle:(NSString*)title action:(SEL)action;
- (NSButton*)checkboxWithTitle:(NSString*)title state:(BOOL)on;
- (NSSlider*)sliderMin:(double)lo max:(double)hi value:(double)v action:(SEL)action;
- (NSView*)row:(NSView*)main trailing:(NSView*)trailing;
- (NSTextField*)numberFieldWithValue:(NSString*)value action:(SEL)action;
- (void)toggleLeftPane:(id)sender;
- (void)toggleRightPane:(id)sender;
- (void)selectInspectorTab:(NSInteger)tab;

// ---- キュー（+Queue） ----
- (void)addPathsToQueue:(NSArray*)paths;
- (void)openFileAtPath:(NSString*)path;
- (void)selectQueueIndex:(NSInteger)index;
- (void)resetWorkspaceForNewProcessing;
- (void)reloadQueueRow:(NSInteger)row;
- (void)fillHeaderInfo:(QueueItem*)item;
- (NSString*)inputPathString;
- (NSString*)inputDisplayName;
- (long long)inputSizeBytes;
- (void)noteRecentPath:(NSString*)path;
- (NSArray*)recentPaths;
- (void)openRecent:(id)sender;
- (void)clearRecent:(id)sender;
- (void)saveQueueState;
- (void)restoreQueueState;
- (void)openDocument:(id)sender;

// ---- 設定（+Settings） ----
- (stackcore::OpenOptions)currentOpenOptions;
- (stackcore::MapStackSettings)currentSettings;
- (NSString*)qualitySignature;
- (NSString*)analysisSignature;
- (BOOL)qualityUsable;
- (BOOL)analysisUsable;
- (BOOL)globalUsable;
- (BOOL)alignmentUsable;
- (void)updateControlsEnabled;
- (void)updateNamePreview;
- (void)updateDrizzleEstimate;
- (void)refreshSelectionControl;
- (void)refreshCutLabel;
- (NSDictionary*)settingsDictionary;
- (void)applySettingsDictionary:(NSDictionary*)d;
- (void)applySettingsDictionary:(NSDictionary*)d includePostProcessing:(BOOL)includePost;
- (void)persistSettings;
- (void)restorePersistedSettings;
- (void)reloadPresets;
- (NSString*)sidecarPath;
- (NSString*)sidecarSettingsPath;
- (NSString*)qualityCachePath;
- (void)saveSidecarForCurrent;
- (void)loadSidecarForCurrent;
- (void)saveQualityCacheForCurrent;
- (BOOL)loadQualityCacheForCurrent;
- (void)invalidateCalibration;
- (void)analysisSettingChanged:(id)sender;
- (void)inputInterpretationChanged:(id)sender;
- (void)topChanged:(id)sender;
- (void)apTopChanged:(id)sender;
- (void)searchRadiusChanged:(id)sender;
- (void)drizzleChanged:(id)sender;
- (void)selectionModeChanged:(id)sender;
- (void)advancedFieldChanged:(id)sender;
- (void)chooseDark:(id)sender;
- (void)chooseFlat:(id)sender;
- (void)clearCalibration:(id)sender;
- (void)presetSelected:(id)sender;
- (void)savePreset:(id)sender;
- (NSString*)analysisSummaryText;

// ---- 実行（+Jobs） ----
- (void)analyze:(id)sender;
- (void)align:(id)sender;
- (void)run:(id)sender;
- (void)cancel:(id)sender;
- (void)beginJobStage:(JobStage)stage;
- (void)startJobStage:(JobStage)stage;
- (NSDictionary*)stackInfoForCurrentSettings;
- (void)finishCalibration:(std::shared_ptr<stackcore::CalibrationFrames>)cal
                    error:(const std::string&)error
                cancelled:(bool)cancelled
                     then:(void (^)(void))next;
- (void)finishJob:(const JobResult&)result
    qualitySignature:(NSString*)qualitySignature
    alignmentSignature:(NSString*)alignmentSignature
    stackInfo:(NSDictionary*)stackInfo;
- (void)reportStage:(NSString*)stage done:(int)done total:(int)total;
- (void)resetEta;
- (NSString*)formatSeconds:(double)seconds;
- (void)notifyDone:(NSString*)text;
- (void)showError:(NSString*)message title:(NSString*)title;
- (void)markCurrentItemSucceeded;
- (void)ensureCalibrationThen:(void (^)(void))next;
- (BOOL)confirmStopForReason:(NSString*)reason;
- (void)jobDidStop;

// ---- プレビュー（+Preview） ----
- (void)showSourceFrame:(int)index;
- (int)currentFrameIndex;
- (void)frameSliderChanged:(id)sender;
- (void)frameOrderChanged:(id)sender;
- (void)graphModeChanged:(id)sender;
- (void)viewModeChanged:(id)sender;
- (void)zoomChanged:(id)sender;
- (void)syncZoomControl;
- (void)displayMenuChanged:(id)sender;
- (void)apDisplayChanged:(id)sender;
- (void)updateApOverlay;
- (void)showFrames:(const std::vector<stackcore::FrameInfo>&)frames;
- (void)rebuildQualityOrder;
- (void)updateFrameInfoLabel;
- (void)updateBanner;
- (void)rebuildReferenceImage;
- (void)useLittleEndianFromBanner:(id)sender;
- (void)useBigEndianFromBanner:(id)sender;
- (void)use12BitFromBanner:(id)sender;
- (void)dismissBanner:(id)sender;
- (void)showRejectedFrames:(id)sender;
- (void)ensureManualPointsInitialized;
- (void)applyManualPoints;
- (void)setManualPointsForUndo:(NSData*)data;
- (void)resetApPlacement:(id)sender;
- (void)clearApPlacement:(id)sender;
- (double)stackedScale;
- (void)zoomIn:(id)sender;
- (void)zoomOut:(id)sender;
- (void)zoomToFit:(id)sender;
- (void)zoomActualPixels:(id)sender;

// ---- 仕上げ・書き出し（+Finishing） ----
- (stackcore::FinishingSettings)currentFinishingSettings;
- (void)resetFinishingForNewStack;
- (void)applyWavelet;
- (void)requestFinishingRender:(BOOL)draft;
- (void)showFinishedOrStacked;
- (void)waveletChanged:(id)sender;
- (void)waveletPreviewChanged:(id)sender;
- (void)resetWavelet:(id)sender;
- (void)finishingChanged:(id)sender;
- (void)updateFinishingValueLabels;
- (void)autoChannelAlign:(id)sender;
- (void)resetChannelAlign:(id)sender;
- (void)autoWhiteBalance:(id)sender;
- (void)resetColor:(id)sender;
- (void)autoTone:(id)sender;
- (void)rotateLeft:(id)sender;
- (void)rotateRight:(id)sender;
- (void)autoCrop:(id)sender;
- (void)clearCrop:(id)sender;
- (OutputFormat)currentOutputFormat;
- (NSString*)outputExtension;
- (NSString*)outputNameForPercentText:(NSString*)selection;
- (NSString*)defaultOutputName;
- (stackcore::ImageMetadata)metadataForExport;
- (void)formatChanged:(id)sender;
- (void)save:(id)sender;
- (void)exportMultiplePercents:(id)sender;
- (void)stretchToggled:(id)sender;
- (std::vector<stackcore::WaveletLayerParams>)waveletParams;
- (void)finishedRender:(std::shared_ptr<stackcore::FrameBuffer>)out
            generation:(long)generation
                 draft:(bool)draft
                 error:(const std::string&)error;
- (std::shared_ptr<stackcore::FrameBuffer>)renderFinishingNow;
- (BOOL)sliderIsDragging;
- (void)captureLinkedProfile:(double*)profile;
- (void)applyChannelOffsets:(stackcore::ChannelOffsets)o;
- (void)applyGainsRed:(double)r blue:(double)b;
- (void)applyToneBlack:(double)black white:(double)white;
- (BOOL)stackedMidTicks:(std::int64_t*)ticks;
- (void)finishMultiExportWritten:(int)written
                           total:(int)total
                          folder:(NSString*)folder
                           error:(const std::string&)error
                       cancelled:(bool)cancelled;
- (int)sliderPositionForFrame:(int)index;
- (void)setFrameOrderByQuality:(BOOL)byQuality;
- (void)updateSliderRange;
- (double)overlayScale;
- (NSData*)manualPointsSnapshot;
- (void)registerManualPointsUndo:(NSString*)actionName;
- (NSString*)inputSignature;
- (void)clearWorkspace:(id)sender;
- (NSInteger)contextQueueRow;
- (void)removeSelectedFromQueue:(id)sender;
- (void)revealQueueItemInFinder:(id)sender;
- (NSInteger)tabIndexForSectionKey:(NSString*)key;
- (void)beginSection:(NSString*)title key:(NSString*)key inBox:(NSStackView*)box;
- (void)addToSection:(NSString*)key view:(NSView*)view box:(NSStackView*)box;
- (BOOL)sectionOpen:(NSString*)key;
- (void)updateSectionHeader:(NSButton*)header key:(NSString*)key;
- (void)toggleSection:(id)sender;
- (NSTextField*)noteLabel:(NSString*)text;
- (NSView*)labeledField:(NSString*)caption field:(NSTextField*)field;
- (NSView*)captionRow:(NSString*)caption slider:(NSSlider*)slider value:(NSTextField*)value;
- (NSStackView*)buttonRow:(NSArray*)buttons;
- (void)buildQualitySection:(NSStackView*)box;
- (void)buildInputSection:(NSStackView*)box;
- (void)buildQualityAdvancedSection:(NSStackView*)box;
- (void)buildAlignmentSection:(NSStackView*)box;
- (void)buildAlignmentAdvancedSection:(NSStackView*)box;
- (void)buildStackSection:(NSStackView*)box;
- (void)buildDrizzleSection:(NSStackView*)box;
- (void)buildChannelSection:(NSStackView*)box;
- (void)buildWaveletSection:(NSStackView*)box;
- (void)buildColorSection:(NSStackView*)box;
- (void)buildToneSection:(NSStackView*)box;
- (void)buildGeometrySection:(NSStackView*)box;
- (void)buildExportSection:(NSStackView*)box;
- (NSString*)chooseCalibrationSourceWithMessage:(NSString*)message;
- (BOOL)pasteboardHasUsableFiles:(NSPasteboard*)pasteboard;
- (void)collectFromDirectory:(NSString*)dir into:(NSMutableArray*)items depth:(int)depth;

// ---- 自己検証（+SelfCheck） ----
- (BOOL)selfCheckApHitTest;
- (BOOL)selfCheckEditingAndPresets;
- (BOOL)selfCheckFrameOrder;
- (BOOL)selfCheckFinishingMatchesExport;

@end

// デリゲートの採用は、そのメソッドを実装するカテゴリで宣言する。
@interface MainWindowController (Queue) <NSTableViewDataSource, NSTableViewDelegate,
                                         NSMenuDelegate, NSMenuItemValidation>
@end

@interface MainWindowController (Settings) <QualityGraphViewDelegate>
@end

@interface MainWindowController (Preview) <PreviewViewDelegate>
@end
