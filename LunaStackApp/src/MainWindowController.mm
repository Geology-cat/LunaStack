#import "MainWindowController.h"

#import "Localization.h"
#import "Presets.h"
#import "QueueItem.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "stackcore/map_pipeline.hpp"
#include "stackcore/png_writer.hpp"
#include "stackcore/tiff_writer.hpp"
#include "stackcore/video_source.hpp"
#include "stackcore/wavelet.hpp"

// 原点が左上のコンテナ。
//
// NSView の座標系は既定で原点が**左下**にある。スクロールビューの
// documentView をそのまま入れると、初期位置が「いちばん下」になり、
// 一覧の先頭ではなく末尾が見えた状態で開いてしまう。
@interface FlippedView : NSView
@end

@implementation FlippedView
- (BOOL)isFlipped {
    return YES;
}
@end

namespace {

// ウェーブレットのレイヤー数。仕様書 §4.10 の既定。
constexpr int kWaveletLayers = 6;

enum class OutputFormat { Tiff16, TiffFloat32, Png16 };

void write_output_image(const std::string& path, const stackcore::FrameBuffer& image,
                        OutputFormat format) {
    if (format == OutputFormat::Png16) {
        stackcore::write_png16(path, image);
        return;
    }
    stackcore::write_tiff(path, image, format == OutputFormat::TiffFloat32
                                           ? stackcore::TiffFormat::Float32
                                           : stackcore::TiffFormat::UInt16);
}

NSTextField* MakeLabel(NSString* text) {
    NSTextField* label = [[[NSTextField alloc] init] autorelease];
    [label setStringValue:text];
    [label setBezeled:NO];
    [label setDrawsBackground:NO];
    [label setEditable:NO];
    [label setSelectable:NO];
    [label setFont:[NSFont systemFontOfSize:11.0]];
    [label setTranslatesAutoresizingMaskIntoConstraints:NO];
    return label;
}

// 1ファイルぶんの処理。
//
// **GUIから完全に切り離してある。** 単発実行もバッチも同じ関数を通るので、
// 「バッチだけ挙動が違う」という食い違いが起きない。
struct JobRequest {
    std::string path;
    stackcore::OpenOptions options;
    stackcore::MapStackSettings settings;
    bool global_only = false;
    bool low_memory = false;
    // 使い回す解析結果。null なら解析からやり直す。
    std::shared_ptr<stackcore::AnalysisData> reuse;
    bool want_stack = true;
};

struct JobResult {
    std::shared_ptr<stackcore::AnalysisData> analysis;  // MAPモードのみ
    std::vector<stackcore::FrameInfo> frames;           // 品質グラフ用
    std::shared_ptr<stackcore::FrameBuffer> image;      // want_stack のとき
    std::string error;
    bool cancelled = false;
    bool reused_analysis = false;
};

JobResult run_job(const JobRequest& req, const stackcore::ProgressFn& progress) {
    JobResult out;
    try {
        std::unique_ptr<stackcore::VideoSource> source =
            stackcore::open_video(req.path, req.options);
        if (req.low_memory) source->set_low_memory(true);

        if (req.global_only) {
            // グローバルのみのモードにはAP変位場が無いので、サイドカーは作らない。
            // そもそも解析の重い部分はMAP段なので、再利用の価値も小さい。
            const stackcore::GlobalStageReport global = stackcore::run_global_stage(
                *source, req.settings.global, req.settings.raw_cfa, progress);
            out.frames = global.frames;
            if (req.want_stack) {
                const std::vector<stackcore::FrameInfo> selected = stackcore::select_top_frames(
                    global.frames, req.settings.reference_top_percent);
                out.image = std::make_shared<stackcore::FrameBuffer>(
                    stackcore::build_global_reference(*source, global, selected,
                                                      req.settings.raw_cfa, progress,
                                                      req.settings.normalize_brightness));
            }
            return out;
        }

        std::shared_ptr<stackcore::AnalysisData> analysis = req.reuse;
        if (analysis) {
            out.reused_analysis = true;
        } else {
            stackcore::MapStackReport report;
            analysis = std::make_shared<stackcore::AnalysisData>(
                stackcore::analyze_map_stack(*source, req.settings, progress, report));
        }
        out.analysis = analysis;
        out.frames = analysis->frames;

        if (req.want_stack) {
            stackcore::MapStackReport report;
            out.image = std::make_shared<stackcore::FrameBuffer>(stackcore::stack_from_analysis(
                *source, req.settings, *analysis, progress, report));
        }
    } catch (const stackcore::Cancelled&) {
        out.cancelled = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

// AP別の平均品質（ヒートマップ用）。行列の並びは points 順 → analyzed_indices 順。
std::vector<double> ap_mean_quality(const stackcore::AnalysisData& analysis) {
    std::vector<double> out;
    const std::size_t ap_count = analysis.points.size();
    const std::size_t frames = analysis.analyzed_indices.size();
    if (ap_count == 0 || frames == 0 || analysis.matrix.size() < ap_count * frames) return out;

    out.resize(ap_count, 0.0);
    for (std::size_t a = 0; a < ap_count; ++a) {
        double sum = 0.0;
        for (std::size_t f = 0; f < frames; ++f) {
            sum += analysis.matrix[a * frames + f].quality;
        }
        out[a] = sum / static_cast<double>(frames);
    }
    return out;
}

}  // namespace

// ブロックの中から呼ぶメソッドは、ここで先に宣言しておく。
// 定義より前に出てくるため、これがないと型が分からず素通りしてしまう。
@interface MainWindowController ()
- (void)reportStage:(NSString*)stage done:(int)done total:(int)total;
- (void)finishJob:(const JobResult&)result
        signature:(NSString*)signature
      wantedStack:(bool)wantedStack;
- (NSString*)queuePathAtIndex:(NSUInteger)index;
- (void)beginBatchItem:(NSUInteger)index position:(NSUInteger)position of:(NSUInteger)count;
- (void)finishBatchItem:(NSUInteger)index
                outPath:(NSString*)outPath
                  error:(NSString*)error
              cancelled:(bool)cancelled;
- (void)finishBatch;
- (NSString*)outputNameForPath:(NSString*)path apSize:(int)apSize;
- (NSString*)outputDirectoryForPath:(NSString*)path;
- (void)updateNamePreview;
- (void)updateDrizzleEstimate;
- (void)updateControlsEnabled;
- (void)applyWavelet;
- (void)updateInspectorVisibility;
- (void)showError:(NSString*)message title:(NSString*)title;
- (NSString*)inputPathString;
- (void)updateBanner;
- (void)showSourceFrame:(int)index;
- (void)rebuildReferenceImage;
- (void)selectQueueIndex:(NSInteger)index;
- (stackcore::OpenOptions)currentOpenOptions;
- (void)applySettingsDictionary:(NSDictionary*)d includePostProcessing:(BOOL)includePost;
- (void)refreshSelectionControl;
@end

@implementation MainWindowController {
    // --- 左ペイン ---
    NSTableView* _queueTable;
    QualityGraphView* _graph;
    NSSegmentedControl* _graphMode;

    // --- 中央 ---
    PreviewView* _preview;
    NSSegmentedControl* _zoomControl;
    NSButton* _apShowCheck;
    NSButton* _apHeatCheck;
    NSButton* _apEditCheck;
    NSTextField* _apCountLabel;
    NSTextField* _bannerLabel;

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
    NSButton* _stretchCheck;

    NSPopUpButton* _formatPopup;
    NSTextField* _outputDirLabel;
    NSTextField* _namePreview;
    NSPopUpButton* _presetPopup;

    // --- 下部 ---
    NSTextField* _statusLabel;
    NSProgressIndicator* _progress;
    NSButton* _analyzeButton;
    NSButton* _stackButton;
    NSButton* _batchButton;
    NSButton* _cancelButton;
    NSButton* _saveButton;

    // セクションの開閉。key → その節に属するビュー。
    NSMutableDictionary* _sections;
    // key → 見出しボタン。タブ切替でセクションごと隠すために持つ。
    NSMutableDictionary* _sectionHeaders;

    // --- 状態 ---
    NSMutableArray* _items;         // QueueItem
    NSInteger _currentIndex;
    NSString* _outputDirectory;     // nil で「入力と同じフォルダ」

    std::string _inputPath;
    int _sourceChannels;
    int _sourceFrames;
    BOOL _byteOrderSuspect;
    BOOL _looksLikeShallowDepth;  // 16bitコンテナに12bitが入っている疑い
    int _rejectedFrames;
    int _sourceWidth;
    int _sourceHeight;
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
    std::atomic<bool>* _stopQueue;
    BOOL _running;
    BOOL _batchRunning;
    int _frameLimit;
    BOOL _selectionUsesCount;
    double _apTopPercentSetting;
    int _apTopCountSetting;

    // 残り時間の推定。フェーズが変わったら測り直す。
    NSString* _etaStage;
    NSTimeInterval _etaStart;
}

@synthesize onRunFinished = _onRunFinished;

- (instancetype)init {
    // 10.13でも使える古い方のスタイルマスク定数を使う。
    const NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    NSWindow* window = [[[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 800)
                                                    styleMask:style
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO] autorelease];
    [window setTitle:@"LunaStack"];
    // UI設計書 §2 の最小サイズ。10.13世代のノートでも収まること。
    [window setMinSize:NSMakeSize(1000, 640)];
    [window center];

    self = [super initWithWindow:window];
    if (!self) return nil;

    _cancelFlag = new std::atomic<bool>(false);
    _stopQueue = new std::atomic<bool>(false);
    _running = NO;
    _batchRunning = NO;
    _frameLimit = 0;
    _selectionUsesCount = NO;
    _apTopPercentSetting = 10.0;
    _apTopCountSetting = 100;
    _currentIndex = -1;
    _manualPointsActive = NO;
    _sourceChannels = 1;
    _sourceFrames = 0;
    _byteOrderSuspect = NO;
    _looksLikeShallowDepth = NO;
    _rejectedFrames = 0;
    _sourceWidth = 0;
    _sourceHeight = 0;
    _etaStart = 0.0;
    _items = [[NSMutableArray alloc] init];
    _sections = [[NSMutableDictionary alloc] init];
    _sectionHeaders = [[NSMutableDictionary alloc] init];
    _etaStage = nil;

    [window setDelegate:self];
    [self buildInterface];
    [self reloadPresets];
    return self;
}

- (void)dealloc {
    delete _cancelFlag;
    delete _stopQueue;
    [_items release];
    [_sections release];
    [_sectionHeaders release];
    [_analysisSignature release];
    [_outputDirectory release];
    [_etaStage release];
    [_onRunFinished release];
    [super dealloc];
}

// ---- 画面の組み立て -------------------------------------------------------

- (void)buildInterface {
    NSView* content = [[self window] contentView];

    // コンテンツビュー自身に背景を持たせる。
    // ウィンドウの背景は本来ウィンドウ枠側が描くので、
    // コンテンツビューだけを画像に落とすと背景が抜ける
    // （自己検証のスナップショットが真っ黒になって気づいた）。
    [content setWantsLayer:YES];
    [[content layer] setBackgroundColor:[[NSColor windowBackgroundColor] CGColor]];

    NSView* left = [self buildLeftPane];
    NSView* center = [self buildCenterPane];
    NSView* right = [self buildRightPane];
    NSView* status = [self buildStatusBar];

    for (NSView* v in @[ left, center, right, status ]) {
        [v setTranslatesAutoresizingMaskIntoConstraints:NO];
        [content addSubview:v];
    }

    // 3ペインは幅を固定＋中央可変にする。
    //
    // NSSplitView を使わないのは、10.13から最新まで同じ見た目で動くことを
    // 優先したため。分割線のドラッグより「どの環境でも壊れない」ことを取る。
    // 左右は⌘1/⌘2で畳める（UI設計書 §2 の「折りたたみ可能」）。
    NSDictionary* views = NSDictionaryOfVariableBindings(left, center, right, status);
    [content addConstraints:[NSLayoutConstraint
                                constraintsWithVisualFormat:
                                    @"H:|-10-[left(220)]-8-[center(>=360)]-8-[right(300)]-10-|"
                                                    options:0
                                                    metrics:nil
                                                      views:views]];
    [content addConstraints:[NSLayoutConstraint
                                constraintsWithVisualFormat:@"V:|-10-[left]-8-[status(46)]-8-|"
                                                    options:0
                                                    metrics:nil
                                                      views:views]];
    [content addConstraints:[NSLayoutConstraint
                                constraintsWithVisualFormat:@"V:|-10-[center]-8-[status]"
                                                    options:0
                                                    metrics:nil
                                                      views:views]];
    [content addConstraints:[NSLayoutConstraint
                                constraintsWithVisualFormat:@"V:|-10-[right]-8-[status]"
                                                    options:0
                                                    metrics:nil
                                                      views:views]];
    [content addConstraints:[NSLayoutConstraint
                                constraintsWithVisualFormat:@"H:|-10-[status]-10-|"
                                                    options:0
                                                    metrics:nil
                                                      views:views]];

    [self updateControlsEnabled];
    [self updateDrizzleEstimate];
    [self updateNamePreview];
    LSLocalizeViewTree(content);
}

// --- 左ペイン: 入力キュー ＋ 品質グラフ ---

- (NSView*)buildLeftPane {
    NSView* pane = [[[NSView alloc] initWithFrame:NSZeroRect] autorelease];

    NSTextField* queueTitle = [self sectionTitle:@"入力キュー"];
    [pane addSubview:queueTitle];

    NSScrollView* scroll = [[[NSScrollView alloc] initWithFrame:NSZeroRect] autorelease];
    [scroll setTranslatesAutoresizingMaskIntoConstraints:NO];
    [scroll setHasVerticalScroller:YES];
    [scroll setBorderType:NSBezelBorder];
    [pane addSubview:scroll];

    _queueTable = [[[NSTableView alloc] initWithFrame:NSZeroRect] autorelease];
    NSTableColumn* col = [[[NSTableColumn alloc] initWithIdentifier:@"file"] autorelease];
    [col setWidth:190.0];
    [_queueTable addTableColumn:col];
    [_queueTable setHeaderView:nil];
    [_queueTable setRowHeight:40.0];
    [_queueTable setDataSource:self];
    [_queueTable setDelegate:self];
    [_queueTable setTarget:self];
    [_queueTable setUsesAlternatingRowBackgroundColors:YES];
    // ドラッグ&ドロップで追加できるようにする（UI設計書 §3.1）。
    // NSFilenamesPboardType は10.14で非推奨なので、10.13から使える
    // NSPasteboardTypeFileURL を指定する。
    [_queueTable registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    [scroll setDocumentView:_queueTable];

    NSStackView* buttons = [[[NSStackView alloc] init] autorelease];
    [buttons setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
    [buttons setSpacing:6.0];
    [buttons setTranslatesAutoresizingMaskIntoConstraints:NO];
    [buttons addArrangedSubview:[self buttonWithTitle:@"追加…" action:@selector(openDocument:)]];
    [buttons addArrangedSubview:[self buttonWithTitle:@"削除"
                                               action:@selector(removeSelectedFromQueue:)]];
    [pane addSubview:buttons];

    NSTextField* graphTitle = [self sectionTitle:@"品質グラフ"];
    [pane addSubview:graphTitle];

    _graphMode = [[[NSSegmentedControl alloc] init] autorelease];
    [_graphMode setSegmentCount:2];
    [_graphMode setLabel:@"時系列" forSegment:0];
    [_graphMode setLabel:@"品質順" forSegment:1];
    [_graphMode setSelectedSegment:0];
    [_graphMode setTarget:self];
    [_graphMode setAction:@selector(graphModeChanged:)];
    [_graphMode setTranslatesAutoresizingMaskIntoConstraints:NO];
    [pane addSubview:_graphMode];

    _graph = [[[QualityGraphView alloc] initWithFrame:NSZeroRect] autorelease];
    [_graph setTranslatesAutoresizingMaskIntoConstraints:NO];
    [_graph setDelegate:self];
    [_graph setCutPercent:25.0];
    [pane addSubview:_graph];

    NSTextField* hint = MakeLabel(@"横線をドラッグすると参照フレームの割合が変わります");
    [hint setTextColor:[NSColor secondaryLabelColor]];
    [hint setLineBreakMode:NSLineBreakByWordWrapping];
    [[hint cell] setWraps:YES];
    [pane addSubview:hint];

    NSDictionary* views = NSDictionaryOfVariableBindings(queueTitle, scroll, buttons, graphTitle,
                                                         _graphMode, _graph, hint);
    for (NSString* format in @[
             @"H:|[queueTitle]|", @"H:|[scroll]|", @"H:|[buttons]|", @"H:|[graphTitle]|",
             @"H:|[_graphMode]|", @"H:|[_graph]|", @"H:|[hint]|"
         ]) {
        [pane addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:format
                                                                    options:0
                                                                    metrics:nil
                                                                      views:views]];
    }
    [pane addConstraints:
              [NSLayoutConstraint
                  constraintsWithVisualFormat:@"V:|[queueTitle]-4-[scroll(>=140)]-4-[buttons]-12-"
                                              @"[graphTitle]-4-[_graphMode]-4-[_graph(120)]-4-"
                                              @"[hint]-(>=0)-|"
                                      options:0
                                      metrics:nil
                                        views:views]];
    return pane;
}

// --- 中央: プレビュー ---

- (NSView*)buildCenterPane {
    NSView* pane = [[[NSView alloc] initWithFrame:NSZeroRect] autorelease];

    _bannerLabel = MakeLabel(@"");
    [_bannerLabel setTextColor:[NSColor systemOrangeColor]];
    [_bannerLabel setLineBreakMode:NSLineBreakByTruncatingTail];
    [pane addSubview:_bannerLabel];

    _preview = [[[PreviewView alloc] initWithFrame:NSZeroRect] autorelease];
    [_preview setTranslatesAutoresizingMaskIntoConstraints:NO];
    [_preview setDelegate:self];
    [pane addSubview:_preview];

    // ツールは2段に分ける。1段に並べると、最小ウィンドウ幅
    // （UI設計書 §2 の 1000pt）に収まらず、ウィンドウが勝手に広がってしまう。
    NSStackView* tools = [[[NSStackView alloc] init] autorelease];
    [tools setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
    [tools setSpacing:8.0];
    [tools setTranslatesAutoresizingMaskIntoConstraints:NO];

    NSStackView* tools2 = [[[NSStackView alloc] init] autorelease];
    [tools2 setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
    [tools2 setSpacing:8.0];
    [tools2 setTranslatesAutoresizingMaskIntoConstraints:NO];

    _viewModeSegment = [[[NSSegmentedControl alloc] init] autorelease];
    [_viewModeSegment setSegmentCount:3];
    [_viewModeSegment setLabel:@"フレーム" forSegment:0];
    [_viewModeSegment setLabel:@"参照" forSegment:1];
    [_viewModeSegment setLabel:@"結果" forSegment:2];
    [_viewModeSegment setSelectedSegment:0];
    [_viewModeSegment setTarget:self];
    [_viewModeSegment setAction:@selector(viewModeChanged:)];
    [_viewModeSegment setTranslatesAutoresizingMaskIntoConstraints:NO];
    [tools addArrangedSubview:_viewModeSegment];

    _frameSlider = [self sliderMin:0.0 max:0.0 value:0.0 action:@selector(frameSliderChanged:)];
    [[_frameSlider widthAnchor] constraintGreaterThanOrEqualToConstant:80.0].active = YES;
    [tools addArrangedSubview:_frameSlider];

    _zoomControl = [[[NSSegmentedControl alloc] init] autorelease];
    [_zoomControl setSegmentCount:4];
    NSArray* zoomLabels = @[ @"全体", @"等倍", @"2倍", @"4倍" ];
    for (NSUInteger i = 0; i < [zoomLabels count]; ++i) {
        [_zoomControl setLabel:zoomLabels[i] forSegment:static_cast<NSInteger>(i)];
    }
    [_zoomControl setSelectedSegment:0];
    [_zoomControl setTarget:self];
    [_zoomControl setAction:@selector(zoomChanged:)];
    [_zoomControl setTranslatesAutoresizingMaskIntoConstraints:NO];
    [tools2 addArrangedSubview:_zoomControl];

    _apShowCheck = [self checkboxWithTitle:@"AP表示" state:YES];
    [_apShowCheck setTarget:self];
    [_apShowCheck setAction:@selector(apDisplayChanged:)];
    [tools2 addArrangedSubview:_apShowCheck];

    _apHeatCheck = [self checkboxWithTitle:@"品質で色分け" state:NO];
    [_apHeatCheck setTarget:self];
    [_apHeatCheck setAction:@selector(apDisplayChanged:)];
    [tools2 addArrangedSubview:_apHeatCheck];

    _apEditCheck = [self checkboxWithTitle:@"AP編集" state:NO];
    [_apEditCheck setTarget:self];
    [_apEditCheck setAction:@selector(apDisplayChanged:)];
    [tools2 addArrangedSubview:_apEditCheck];

    _apCountLabel = MakeLabel(@"");
    [_apCountLabel setTextColor:[NSColor secondaryLabelColor]];
    [_apCountLabel setLineBreakMode:NSLineBreakByTruncatingTail];
    // 幅が足りないときに真っ先に縮むのはこのラベルでよい。
    [_apCountLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow - 1
                                            forOrientation:NSLayoutConstraintOrientationHorizontal];
    [tools2 addArrangedSubview:_apCountLabel];

    [pane addSubview:tools];
    [pane addSubview:tools2];

    NSDictionary* views =
        NSDictionaryOfVariableBindings(_bannerLabel, _preview, tools, tools2);
    for (NSString* format in
         @[ @"H:|[_bannerLabel]|", @"H:|[_preview]|", @"H:|[tools]-(>=0)-|",
            @"H:|[tools2]-(>=0)-|" ]) {
        [pane addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:format
                                                                    options:0
                                                                    metrics:nil
                                                                      views:views]];
    }
    [pane addConstraints:[NSLayoutConstraint
                             constraintsWithVisualFormat:
                                 @"V:|[_bannerLabel(16)]-4-[_preview(>=240)]-6-[tools]-4-[tools2]|"
                                                 options:0
                                                 metrics:nil
                                                   views:views]];
    return pane;
}

// --- 右: Inspector ---

- (NSView*)buildRightPane {
    NSView* pane = [[[NSView alloc] initWithFrame:NSZeroRect] autorelease];

    // 工程タブ。UI設計書 §6 のワークフロー（解析→スタック→仕上げ→書き出し）を
    // UIの構造にも出す。全セクションを1列に積むと「今どの段階の設定を
    // 触っているのか」が分からなくなる、という声への対応。
    _inspectorTab = [[[NSSegmentedControl alloc] init] autorelease];
    [_inspectorTab setSegmentCount:2];
    [_inspectorTab setLabel:@"1. 解析・スタック" forSegment:0];
    [_inspectorTab setLabel:@"2. 仕上げ・書き出し" forSegment:1];
    [_inspectorTab setSelectedSegment:0];
    [_inspectorTab setTarget:self];
    [_inspectorTab setAction:@selector(inspectorTabChanged:)];
    [_inspectorTab setTranslatesAutoresizingMaskIntoConstraints:NO];
    [pane addSubview:_inspectorTab];

    NSScrollView* scroll = [[[NSScrollView alloc] initWithFrame:NSZeroRect] autorelease];
    [scroll setHasVerticalScroller:YES];
    [scroll setDrawsBackground:NO];
    [scroll setTranslatesAutoresizingMaskIntoConstraints:NO];
    [pane addSubview:scroll];

    NSDictionary* views = NSDictionaryOfVariableBindings(_inspectorTab, scroll);
    for (NSString* format in @[ @"H:|[_inspectorTab]|", @"H:|[scroll]|",
                                @"V:|[_inspectorTab]-6-[scroll]|" ]) {
        [pane addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:format
                                                                    options:0
                                                                    metrics:nil
                                                                      views:views]];
    }

    FlippedView* container = [[[FlippedView alloc] initWithFrame:NSZeroRect] autorelease];
    [container setTranslatesAutoresizingMaskIntoConstraints:NO];
    [scroll setDocumentView:container];

    NSStackView* box = [[[NSStackView alloc] init] autorelease];
    [box setOrientation:NSUserInterfaceLayoutOrientationVertical];
    [box setAlignment:NSLayoutAttributeLeading];
    [box setSpacing:6.0];
    [box setTranslatesAutoresizingMaskIntoConstraints:NO];
    [container addSubview:box];

    [[[container widthAnchor] constraintEqualToAnchor:[scroll widthAnchor]] setActive:YES];
    [[[box topAnchor] constraintEqualToAnchor:[container topAnchor] constant:2.0] setActive:YES];
    [[[box leadingAnchor] constraintEqualToAnchor:[container leadingAnchor]
                                         constant:2.0] setActive:YES];
    [[[container bottomAnchor] constraintEqualToAnchor:[box bottomAnchor]
                                              constant:4.0] setActive:YES];
    [[[box widthAnchor] constraintEqualToConstant:278.0] setActive:YES];

    [self buildAlignmentSection:box];
    [self buildQualitySection:box];
    [self buildStackSection:box];
    [self buildDrizzleSection:box];
    [self buildWaveletSection:box];
    [self buildExportSection:box];

    [self updateInspectorVisibility];
    return pane;
}

// セクションがどちらの工程タブに属するか。
// 前半＝解析とスタックの設定、後半＝出来上がった結果に対する操作。
- (NSInteger)tabIndexForSectionKey:(NSString*)key {
    if ([key isEqualToString:@"wavelet"] || [key isEqualToString:@"export"]) return 1;
    return 0;
}

- (void)inspectorTabChanged:(id)sender {
    (void)sender;
    [self updateInspectorVisibility];
}

// タブとセクション開閉の両方を見て、Inspectorの表示を組み立て直す。
// 「隠す条件」を一箇所にまとめないと、タブ切替と開閉が互いの状態を壊す。
- (void)updateInspectorVisibility {
    const NSInteger tab = [_inspectorTab selectedSegment];
    for (NSString* key in _sections) {
        const BOOL inTab = ([self tabIndexForSectionKey:key] == tab);
        [(NSButton*)_sectionHeaders[key] setHidden:!inTab];
        const BOOL visible = inTab && [self sectionOpen:key];
        for (NSView* v in _sections[key]) [v setHidden:!visible];
    }
}

// セクションの見出し。押すと中身を畳む。開閉状態は次回起動まで覚える。
- (void)beginSection:(NSString*)title key:(NSString*)key inBox:(NSStackView*)box {
    NSButton* header = [[[NSButton alloc] init] autorelease];
    [header setButtonType:NSButtonTypeMomentaryChange];
    [header setBordered:NO];
    [header setAlignment:NSTextAlignmentLeft];
    [header setFont:[NSFont boldSystemFontOfSize:12.0]];
    [header setTarget:self];
    [header setAction:@selector(toggleSection:)];
    [header setIdentifier:key];
    [header setTranslatesAutoresizingMaskIntoConstraints:NO];
    [box addArrangedSubview:header];

    _sections[key] = [NSMutableArray array];
    _sectionHeaders[key] = header;
    [self updateSectionHeader:header key:key];
}

- (void)addToSection:(NSString*)key view:(NSView*)view box:(NSStackView*)box {
    [box addArrangedSubview:view];
    [_sections[key] addObject:view];
    if (![self sectionOpen:key]) [view setHidden:YES];
}

- (BOOL)sectionOpen:(NSString*)key {
    NSString* defaultsKey = [@"section." stringByAppendingString:key];
    id value = [[NSUserDefaults standardUserDefaults] objectForKey:defaultsKey];
    if (!value) return YES;  // 既定は開いた状態
    return [value boolValue];
}

- (void)updateSectionHeader:(NSButton*)header key:(NSString*)key {
    NSString* mark = [self sectionOpen:key] ? @"▼" : @"▶";
    NSString* title = @"";
    if ([key isEqualToString:@"align"]) title = @"Alignment";
    else if ([key isEqualToString:@"quality"]) title = @"Quality";
    else if ([key isEqualToString:@"stack"]) title = @"Stack";
    else if ([key isEqualToString:@"drizzle"]) title = @"Drizzle";
    else if ([key isEqualToString:@"wavelet"]) title = @"Wavelet";
    else if ([key isEqualToString:@"export"]) title = @"Export";
    [header setTitle:[NSString stringWithFormat:@"%@ %@", mark, title]];
}

- (void)toggleSection:(id)sender {
    NSButton* header = (NSButton*)sender;
    NSString* key = [header identifier];
    const BOOL open = ![self sectionOpen:key];
    [[NSUserDefaults standardUserDefaults] setBool:open
                                            forKey:[@"section." stringByAppendingString:key]];
    [self updateInspectorVisibility];
    [self updateSectionHeader:header key:key];
}

- (void)buildAlignmentSection:(NSStackView*)box {
    NSString* key = @"align";
    [self beginSection:@"Alignment" key:key inBox:box];

    [self addToSection:key view:MakeLabel(@"対象モード") box:box];
    _modeSegment = [[[NSSegmentedControl alloc] init] autorelease];
    [_modeSegment setSegmentCount:3];
    [_modeSegment setLabel:@"自動" forSegment:0];
    [_modeSegment setLabel:@"惑星" forSegment:1];
    [_modeSegment setLabel:@"月・太陽" forSegment:2];
    [_modeSegment setSelectedSegment:0];
    [_modeSegment setTarget:self];
    [_modeSegment setAction:@selector(analysisSettingChanged:)];
    [_modeSegment setTranslatesAutoresizingMaskIntoConstraints:NO];
    [self addToSection:key view:_modeSegment box:box];

    [self addToSection:key view:MakeLabel(@"AP サイズ") box:box];
    _apSizePopup = [[[NSPopUpButton alloc] init] autorelease];
    [_apSizePopup addItemWithTitle:@"自動"];
    for (NSString* s in @[ @"32", @"48", @"64", @"96", @"128", @"200" ]) {
        [_apSizePopup addItemWithTitle:[s stringByAppendingString:@" px"]];
    }
    [_apSizePopup setTarget:self];
    [_apSizePopup setAction:@selector(analysisSettingChanged:)];
    [_apSizePopup setTranslatesAutoresizingMaskIntoConstraints:NO];
    [self addToSection:key view:_apSizePopup box:box];

    [self addToSection:key view:MakeLabel(@"探索半径") box:box];
    _searchRadiusSlider = [self sliderMin:8.0 max:32.0 value:16.0
                                   action:@selector(searchRadiusChanged:)];
    _searchRadiusValue = MakeLabel(@"±16");
    [self addToSection:key
                  view:[self row:_searchRadiusSlider trailing:_searchRadiusValue]
                   box:box];

    [self addToSection:key view:MakeLabel(@"AP配置") box:box];
    NSStackView* apButtons = [[[NSStackView alloc] init] autorelease];
    [apButtons setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
    [apButtons setSpacing:6.0];
    [apButtons setTranslatesAutoresizingMaskIntoConstraints:NO];
    [apButtons addArrangedSubview:[self buttonWithTitle:@"自動配置に戻す"
                                                 action:@selector(resetApPlacement:)]];
    [apButtons addArrangedSubview:[self buttonWithTitle:@"すべて消去"
                                                 action:@selector(clearApPlacement:)]];
    [self addToSection:key view:apButtons box:box];

    [self addToSection:key view:MakeLabel(@"参照フレーム（品質上位 %）") box:box];
    _topSlider = [self sliderMin:5.0 max:50.0 value:25.0 action:@selector(topChanged:)];
    _topValue = MakeLabel(@"25 %");
    [self addToSection:key view:[self row:_topSlider trailing:_topValue] box:box];

    _refineCheck = [self checkboxWithTitle:@"参照の反復精密化（2パス）" state:YES];
    [_refineCheck setTarget:self];
    [_refineCheck setAction:@selector(analysisSettingChanged:)];
    [self addToSection:key view:_refineCheck box:box];
}

- (void)buildQualitySection:(NSStackView*)box {
    NSString* key = @"quality";
    [self beginSection:@"Quality" key:key inBox:box];

    [self addToSection:key view:MakeLabel(@"品質指標") box:box];
    _qualityMetricPopup = [[[NSPopUpButton alloc] init] autorelease];
    [_qualityMetricPopup addItemWithTitle:@"勾配エネルギー"];
    [_qualityMetricPopup addItemWithTitle:@"周波数帯パワー比"];
    [_qualityMetricPopup setTarget:self];
    [_qualityMetricPopup setAction:@selector(analysisSettingChanged:)];
    [_qualityMetricPopup setTranslatesAutoresizingMaskIntoConstraints:NO];
    [self addToSection:key view:_qualityMetricPopup box:box];

    [self addToSection:key view:MakeLabel(@"選択方式") box:box];
    _selectionModeSegment = [[[NSSegmentedControl alloc] initWithFrame:NSZeroRect] autorelease];
    [_selectionModeSegment setSegmentCount:2];
    [_selectionModeSegment setLabel:@"割合" forSegment:0];
    [_selectionModeSegment setLabel:@"枚数" forSegment:1];
    [_selectionModeSegment setSelectedSegment:0];
    [_selectionModeSegment setTarget:self];
    [_selectionModeSegment setAction:@selector(selectionModeChanged:)];
    [_selectionModeSegment setTranslatesAutoresizingMaskIntoConstraints:NO];
    [self addToSection:key view:_selectionModeSegment box:box];

    _apTopCaption = MakeLabel(@"AP別に採用するフレーム（%）");
    [self addToSection:key view:_apTopCaption box:box];
    _apTopSlider = [self sliderMin:1.0 max:100.0 value:10.0 action:@selector(apTopChanged:)];
    _apTopValue = MakeLabel(@"10 %");
    [self addToSection:key view:[self row:_apTopSlider trailing:_apTopValue] box:box];

    NSTextField* note = MakeLabel(@"これだけを変えた再スタックは解析をやり直しません");
    [note setTextColor:[NSColor secondaryLabelColor]];
    [[note cell] setWraps:YES];
    [self addToSection:key view:note box:box];
}

- (void)buildStackSection:(NSStackView*)box {
    NSString* key = @"stack";
    [self beginSection:@"Stack" key:key inBox:box];

    [self addToSection:key view:MakeLabel(@"処理") box:box];
    _methodPopup = [[[NSPopUpButton alloc] init] autorelease];
    [_methodPopup addItemWithTitle:@"MAP局所アライメント（推奨）"];
    [_methodPopup addItemWithTitle:@"グローバルのみ（高速）"];
    [_methodPopup setTranslatesAutoresizingMaskIntoConstraints:NO];
    [_methodPopup setTarget:self];
    [_methodPopup setAction:@selector(analysisSettingChanged:)];
    [self addToSection:key view:_methodPopup box:box];

    _normalizeCheck = [self checkboxWithTitle:@"輝度正規化" state:YES];
    [self addToSection:key view:_normalizeCheck box:box];

    [self addToSection:key view:MakeLabel(@"加算方式") box:box];
    _stackModePopup = [[[NSPopUpButton alloc] init] autorelease];
    [_stackModePopup addItemWithTitle:@"単純平均"];
    [_stackModePopup addItemWithTitle:@"品質重み付き平均"];
    [_stackModePopup addItemWithTitle:@"σクリップ"];
    [_stackModePopup setTarget:self];
    [_stackModePopup setAction:@selector(analysisSettingChanged:)];
    [_stackModePopup setTranslatesAutoresizingMaskIntoConstraints:NO];
    [self addToSection:key view:_stackModePopup box:box];

    _lowMemoryCheck = [self checkboxWithTitle:@"低メモリモード（2GB上限）" state:NO];
    [self addToSection:key view:_lowMemoryCheck box:box];

    // 画像が破綻して見えるときに真っ先に触る2つ（UI設計書 §7.2）。
    // 警告バナーから操作先が無いと、バナーを出す意味がない。
    [self addToSection:key view:MakeLabel(@"バイトオーダー（SERのみ）") box:box];
    _endianPopup = [[[NSPopUpButton alloc] init] autorelease];
    for (NSString* t in @[ @"自動判定", @"little を使う", @"big を使う" ]) {
        [_endianPopup addItemWithTitle:t];
    }
    [_endianPopup setTranslatesAutoresizingMaskIntoConstraints:NO];
    [_endianPopup setTarget:self];
    [_endianPopup setAction:@selector(inputInterpretationChanged:)];
    [self addToSection:key view:_endianPopup box:box];

    [self addToSection:key view:MakeLabel(@"ビット深度の解釈") box:box];
    _depthPopup = [[[NSPopUpButton alloc] init] autorelease];
    for (NSString* t in @[ @"ヘッダに従う", @"12bit として扱う", @"14bit として扱う" ]) {
        [_depthPopup addItemWithTitle:t];
    }
    [_depthPopup setTranslatesAutoresizingMaskIntoConstraints:NO];
    [_depthPopup setTarget:self];
    [_depthPopup setAction:@selector(inputInterpretationChanged:)];
    [self addToSection:key view:_depthPopup box:box];
}

- (void)buildDrizzleSection:(NSStackView*)box {
    NSString* key = @"drizzle";
    [self beginSection:@"Drizzle" key:key inBox:box];

    _drizzleSegment = [[[NSSegmentedControl alloc] init] autorelease];
    [_drizzleSegment setSegmentCount:4];
    NSArray* labels = @[ @"1×", @"1.5×", @"2×", @"3×" ];
    for (NSUInteger i = 0; i < [labels count]; ++i) {
        [_drizzleSegment setLabel:labels[i] forSegment:static_cast<NSInteger>(i)];
    }
    [_drizzleSegment setSelectedSegment:0];
    [_drizzleSegment setTarget:self];
    [_drizzleSegment setAction:@selector(drizzleChanged:)];
    [_drizzleSegment setTranslatesAutoresizingMaskIntoConstraints:NO];
    [self addToSection:key view:_drizzleSegment box:box];

    [self addToSection:key view:MakeLabel(@"pixfrac") box:box];
    _pixfracSlider = [self sliderMin:0.5 max:1.0 value:0.9 action:@selector(drizzleChanged:)];
    _pixfracValue = MakeLabel(@"0.90");
    [self addToSection:key view:[self row:_pixfracSlider trailing:_pixfracValue] box:box];

    _drizzleEstimate = MakeLabel(@"");
    [_drizzleEstimate setTextColor:[NSColor secondaryLabelColor]];
    [self addToSection:key view:_drizzleEstimate box:box];

    // 設計原則1.4「UIが黙って期待を持たせない」。常時出す。
    NSTextField* warn =
        MakeLabel(@"Drizzleが効くのは撮像がアンダーサンプリングの場合だけです。"
                  @"すでにオーバーサンプリング気味なら、倍率を上げても解像度は上がらず、"
                  @"ファイルサイズと処理時間だけが増えます。");
    [warn setTextColor:[NSColor secondaryLabelColor]];
    [[warn cell] setWraps:YES];
    [warn setPreferredMaxLayoutWidth:270.0];
    [self addToSection:key view:warn box:box];
}

- (void)buildWaveletSection:(NSStackView*)box {
    NSString* key = @"wavelet";
    [self beginSection:@"Wavelet" key:key inBox:box];

    NSTextField* head = MakeLabel(@"上=Sharpen / 下=Denoise（結果に即反映）");
    [head setTextColor:[NSColor secondaryLabelColor]];
    [self addToSection:key view:head box:box];

    for (int j = 0; j < kWaveletLayers; ++j) {
        NSString* title = [NSString stringWithFormat:
                                        LSLocalizedString(@"Layer %d（約%d px）"),
                                        j + 1, 1 << (j + 1)];
        [self addToSection:key view:MakeLabel(title) box:box];

        _sharpenSliders[j] = [self sliderMin:0.0 max:3.0 value:1.0
                                      action:@selector(waveletChanged:)];
        _sharpenValues[j] = MakeLabel(@"1.00");
        [self addToSection:key
                      view:[self row:_sharpenSliders[j] trailing:_sharpenValues[j]]
                       box:box];

        _denoiseSliders[j] = [self sliderMin:0.0 max:1.0 value:0.0
                                      action:@selector(waveletChanged:)];
        _denoiseValues[j] = MakeLabel(@"0.00");
        [self addToSection:key
                      view:[self row:_denoiseSliders[j] trailing:_denoiseValues[j]]
                       box:box];
    }

    _stretchCheck = [self checkboxWithTitle:@"表示を明るくする（保存には影響しません）" state:YES];
    [_stretchCheck setTarget:self];
    [_stretchCheck setAction:@selector(stretchToggled:)];
    [self addToSection:key view:_stretchCheck box:box];

    [self addToSection:key
                  view:[self buttonWithTitle:@"すべてリセット" action:@selector(resetWavelet:)]
                   box:box];

    // プリセット（仕様書 §5.1）。
    NSStackView* presetRow = [[[NSStackView alloc] init] autorelease];
    [presetRow setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
    [presetRow setSpacing:6.0];
    [presetRow setTranslatesAutoresizingMaskIntoConstraints:NO];
    _presetPopup = [[[NSPopUpButton alloc] init] autorelease];
    [_presetPopup setTranslatesAutoresizingMaskIntoConstraints:NO];
    [_presetPopup setTarget:self];
    [_presetPopup setAction:@selector(presetSelected:)];
    [presetRow addArrangedSubview:_presetPopup];
    [presetRow addArrangedSubview:[self buttonWithTitle:@"保存…"
                                                 action:@selector(savePreset:)]];
    [self addToSection:key view:presetRow box:box];
}

- (void)buildExportSection:(NSStackView*)box {
    NSString* key = @"export";
    [self beginSection:@"Export" key:key inBox:box];

    [self addToSection:key view:MakeLabel(@"形式") box:box];
    _formatPopup = [[[NSPopUpButton alloc] init] autorelease];
    [_formatPopup addItemWithTitle:@"16bit TIFF"];
    [_formatPopup addItemWithTitle:@"32bit float TIFF"];
    [_formatPopup addItemWithTitle:@"16bit PNG"];
    [_formatPopup setTranslatesAutoresizingMaskIntoConstraints:NO];
    [_formatPopup setTarget:self];
    [_formatPopup setAction:@selector(formatChanged:)];
    [self addToSection:key view:_formatPopup box:box];

    [self addToSection:key view:MakeLabel(@"出力先") box:box];
    _outputDirLabel = MakeLabel(@"入力と同じフォルダ");
    [_outputDirLabel setLineBreakMode:NSLineBreakByTruncatingMiddle];
    [self addToSection:key view:_outputDirLabel box:box];
    NSStackView* dirRow = [[[NSStackView alloc] init] autorelease];
    [dirRow setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
    [dirRow setSpacing:6.0];
    [dirRow setTranslatesAutoresizingMaskIntoConstraints:NO];
    [dirRow addArrangedSubview:[self buttonWithTitle:@"選ぶ…"
                                              action:@selector(chooseOutputDirectory:)]];
    [dirRow addArrangedSubview:[self buttonWithTitle:@"入力と同じ"
                                              action:@selector(resetOutputDirectory:)]];
    [self addToSection:key view:dirRow box:box];

    [self addToSection:key view:MakeLabel(@"ファイル名") box:box];
    _namePreview = MakeLabel(@"");
    [_namePreview setTextColor:[NSColor secondaryLabelColor]];
    [_namePreview setLineBreakMode:NSLineBreakByTruncatingMiddle];
    [self addToSection:key view:_namePreview box:box];

    _saveButton = [self buttonWithTitle:@"名前を付けて書き出し…" action:@selector(save:)];
    [_saveButton setEnabled:NO];
    [self addToSection:key view:_saveButton box:box];
}

// --- 下部ステータスバー ---

- (NSView*)buildStatusBar {
    NSView* bar = [[[NSView alloc] initWithFrame:NSZeroRect] autorelease];

    _analyzeButton = [self buttonWithTitle:@"解析" action:@selector(analyze:)];
    _stackButton = [self buttonWithTitle:@"スタック" action:@selector(run:)];
    [_stackButton setKeyEquivalent:@"\r"];
    _batchButton = [self buttonWithTitle:@"すべて処理" action:@selector(batch:)];
    _cancelButton = [self buttonWithTitle:@"中断" action:@selector(cancel:)];
    [_cancelButton setEnabled:NO];

    _progress = [[[NSProgressIndicator alloc] init] autorelease];
    [_progress setStyle:NSProgressIndicatorStyleBar];
    [_progress setIndeterminate:NO];
    [_progress setMinValue:0.0];
    [_progress setMaxValue:1.0];
    [_progress setHidden:YES];
    [_progress setTranslatesAutoresizingMaskIntoConstraints:NO];

    _statusLabel = MakeLabel(@"動画を追加してください");
    [_statusLabel setLineBreakMode:NSLineBreakByTruncatingTail];

    for (NSView* v in @[ _analyzeButton, _stackButton, _batchButton, _cancelButton, _progress,
                         _statusLabel ]) {
        [bar addSubview:v];
    }

    NSDictionary* views = NSDictionaryOfVariableBindings(_analyzeButton, _stackButton,
                                                         _batchButton, _cancelButton, _progress,
                                                         _statusLabel);
    [bar addConstraints:[NSLayoutConstraint
                            constraintsWithVisualFormat:
                                @"H:|[_analyzeButton(70)]-6-[_stackButton(80)]-6-[_batchButton(90)"
                                @"]-12-[_progress(>=120)]-10-[_statusLabel(>=180)]-8-"
                                @"[_cancelButton(60)]|"
                                                options:NSLayoutFormatAlignAllCenterY
                                                metrics:nil
                                                  views:views]];
    [bar addConstraints:[NSLayoutConstraint
                            constraintsWithVisualFormat:@"V:|-8-[_analyzeButton]-(>=0)-|"
                                                options:0
                                                metrics:nil
                                                  views:views]];
    [bar addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:@"V:[_progress(14)]"
                                                                options:0
                                                                metrics:nil
                                                                  views:views]];
    return bar;
}

// ---- 部品づくり -----------------------------------------------------------

- (NSTextField*)sectionTitle:(NSString*)text {
    NSTextField* label = MakeLabel(text);
    [label setFont:[NSFont boldSystemFontOfSize:12.0]];
    return label;
}

- (NSButton*)buttonWithTitle:(NSString*)title action:(SEL)action {
    NSButton* b = [[[NSButton alloc] init] autorelease];
    [b setTitle:title];
    [b setBezelStyle:NSBezelStyleRounded];
    [b setFont:[NSFont systemFontOfSize:11.0]];
    [b setTarget:self];
    [b setAction:action];
    [b setTranslatesAutoresizingMaskIntoConstraints:NO];
    return b;
}

- (NSButton*)checkboxWithTitle:(NSString*)title state:(BOOL)on {
    NSButton* b = [[[NSButton alloc] init] autorelease];
    [b setButtonType:NSButtonTypeSwitch];
    [b setTitle:title];
    [b setFont:[NSFont systemFontOfSize:11.0]];
    [b setState:on ? NSControlStateValueOn : NSControlStateValueOff];
    [b setTranslatesAutoresizingMaskIntoConstraints:NO];
    return b;
}

- (NSSlider*)sliderMin:(double)lo max:(double)hi value:(double)v action:(SEL)action {
    NSSlider* s = [[[NSSlider alloc] init] autorelease];
    [s setMinValue:lo];
    [s setMaxValue:hi];
    [s setDoubleValue:v];
    [s setTarget:self];
    [s setAction:action];
    [s setContinuous:YES];
    [s setTranslatesAutoresizingMaskIntoConstraints:NO];
    return s;
}

- (NSView*)row:(NSView*)main trailing:(NSView*)trailing {
    NSStackView* row = [[[NSStackView alloc] init] autorelease];
    [row setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
    [row setSpacing:6.0];
    [row setTranslatesAutoresizingMaskIntoConstraints:NO];
    [row addArrangedSubview:main];
    [row addArrangedSubview:trailing];
    [[trailing widthAnchor] constraintEqualToConstant:46.0].active = YES;
    return row;
}

// ---- キュー ---------------------------------------------------------------

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
    (void)tableView;
    return static_cast<NSInteger>([_items count]);
}

- (NSView*)tableView:(NSTableView*)tableView
    viewForTableColumn:(NSTableColumn*)column
                   row:(NSInteger)row {
    (void)tableView;
    (void)column;
    QueueItem* item = _items[static_cast<NSUInteger>(row)];

    NSTextField* field = [[[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 190, 40)] autorelease];
    [field setBezeled:NO];
    [field setDrawsBackground:NO];
    [field setEditable:NO];
    [field setSelectable:NO];
    [[field cell] setWraps:YES];

    NSMutableParagraphStyle* style = [[[NSMutableParagraphStyle alloc] init] autorelease];
    [style setLineBreakMode:NSLineBreakByTruncatingMiddle];

    NSMutableAttributedString* text = [[[NSMutableAttributedString alloc] init] autorelease];
    [text appendAttributedString:
              [[[NSAttributedString alloc]
                  initWithString:[NSString stringWithFormat:@"%@  ", [item stateSymbol]]
                      attributes:@{
                          NSForegroundColorAttributeName : [item stateColor],
                          NSFontAttributeName : [NSFont boldSystemFontOfSize:10.0]
                      }] autorelease]];
    [text appendAttributedString:
              [[[NSAttributedString alloc]
                  initWithString:[[item path] lastPathComponent]
                      attributes:@{
                          NSForegroundColorAttributeName : [NSColor labelColor],
                          NSFontAttributeName : [NSFont systemFontOfSize:11.0],
                          NSParagraphStyleAttributeName : style
                      }] autorelease]];
    NSString* sub = [[item message] length] > 0 ? [item message] : [item subtitle];
    if ([sub length] > 0) {
        [text appendAttributedString:
                  [[[NSAttributedString alloc]
                      initWithString:[@"\n" stringByAppendingString:sub]
                          attributes:@{
                              NSForegroundColorAttributeName : [NSColor secondaryLabelColor],
                              NSFontAttributeName : [NSFont systemFontOfSize:9.0],
                              NSParagraphStyleAttributeName : style
                          }] autorelease]];
    }
    [field setAttributedStringValue:text];
    return field;
}

- (void)tableViewSelectionDidChange:(NSNotification*)notification {
    (void)notification;
    // setEnabled:NO でも選択が動く経路（バッチ側からの選択など）があるので、
    // ここでも実行中は入力を切り替えない。
    if (_running) return;
    const NSInteger row = [_queueTable selectedRow];
    if (row < 0 || row == _currentIndex) return;
    [self selectQueueIndex:row];
}

- (NSDragOperation)tableView:(NSTableView*)tableView
                validateDrop:(id<NSDraggingInfo>)info
                 proposedRow:(NSInteger)row
       proposedDropOperation:(NSTableViewDropOperation)op {
    (void)tableView;
    (void)info;
    (void)row;
    (void)op;
    [_queueTable setDropRow:static_cast<NSInteger>([_items count])
              dropOperation:NSTableViewDropAbove];
    return NSDragOperationCopy;
}

- (BOOL)tableView:(NSTableView*)tableView
       acceptDrop:(id<NSDraggingInfo>)info
              row:(NSInteger)row
    dropOperation:(NSTableViewDropOperation)op {
    (void)tableView;
    (void)row;
    (void)op;
    NSArray* urls = [[info draggingPasteboard]
        readObjectsForClasses:@[ [NSURL class] ]
                      options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    NSMutableArray* paths = [NSMutableArray array];
    for (NSURL* url in urls) [paths addObject:[url path]];
    [self addPathsToQueue:paths];
    return YES;
}

- (void)addPathsToQueue:(NSArray*)paths {
    NSFileManager* fm = [NSFileManager defaultManager];
    NSMutableArray* files = [NSMutableArray array];

    for (NSString* path in paths) {
        BOOL isDir = NO;
        if (![fm fileExistsAtPath:path isDirectory:&isDir]) continue;
        if (isDir) {
            // フォルダを落としたら中のSER/AVIを再帰的に拾う（UI設計書 §3.1）。
            NSDirectoryEnumerator* e = [fm enumeratorAtPath:path];
            for (NSString* rel in e) {
                NSString* ext = [[rel pathExtension] lowercaseString];
                if ([ext isEqualToString:@"ser"] || [ext isEqualToString:@"avi"]) {
                    [files addObject:[path stringByAppendingPathComponent:rel]];
                }
            }
        } else {
            [files addObject:path];
        }
    }
    [files sortUsingSelector:@selector(compare:)];

    for (NSString* file in files) {
        BOOL duplicate = NO;
        for (QueueItem* item in _items) {
            if ([[item path] isEqualToString:file]) duplicate = YES;
        }
        if (duplicate) continue;
        QueueItem* item = [QueueItem itemWithPath:file];
        [self fillHeaderInfo:item];
        [_items addObject:item];
    }
    [_queueTable reloadData];
    if ([_items count] > 0 && _currentIndex < 0) {
        [self selectQueueIndex:0];
    }
    [self updateControlsEnabled];
}

// ヘッダだけ読んで副題を埋める。中身の妥当性はここで分かる。
- (void)fillHeaderInfo:(QueueItem*)item {
    try {
        const std::unique_ptr<stackcore::VideoSource> source =
            stackcore::open_video(std::string([[item path] UTF8String]),
                                  [self currentOpenOptions]);
        [item setSubtitle:[NSString stringWithFormat:LSLocalizedString(@"%dフレーム · %d×%d · %@"),
                                                     source->frame_count(), source->width(),
                                                     source->height(),
                                                     LSLocalizedString([NSString
                                                         stringWithUTF8String:
                                                             source->format_name()])]];
    } catch (const std::exception& e) {
        [item setState:QueueItemStateError];
        [item setMessage:[NSString stringWithUTF8String:e.what()]];
    }
}

- (void)openFileAtPath:(NSString*)path {
    [self addPathsToQueue:@[ path ]];
    for (NSUInteger i = 0; i < [_items count]; ++i) {
        if ([[_items[i] path] isEqualToString:path]) {
            [self selectQueueIndex:static_cast<NSInteger>(i)];
            break;
        }
    }
}

- (void)selectQueueIndex:(NSInteger)index {
    if (index < 0 || index >= static_cast<NSInteger>([_items count])) return;
    _currentIndex = index;
    [_queueTable selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(index)]
             byExtendingSelection:NO];

    QueueItem* item = _items[static_cast<NSUInteger>(index)];
    _inputPath = std::string([[item path] UTF8String]);

    // 選んだファイルが変わったら、前のファイルの解析結果は使えない。
    _analysis.reset();
    [_analysisSignature release];
    _analysisSignature = nil;
    _manualPointsActive = NO;
    _manualPoints.clear();
    _stacked.reset();
    _displayed.reset();
    _wavelet.reset();
    [_graph clearData];
    [_preview clearAlignmentPoints];
    [_bannerLabel setStringValue:@""];

    _referenceImage.reset();
    _rejectedFrames = 0;
    _byteOrderSuspect = NO;
    _looksLikeShallowDepth = NO;
    [_viewModeSegment setSelectedSegment:0];
    // 新しいファイルを選んだら工程を最初に戻す。
    // 仕上げタブのまま別ファイルに切り替わると、結果の無いファイルに対して
    // ウェーブレットのつまみだけが見えている、という宙ぶらりんな画面になる。
    [_inspectorTab setSelectedSegment:0];
    [self updateInspectorVisibility];

    // 1枚目を出しておく。何も映らないより、まず見えたほうがよい。
    try {
        const std::unique_ptr<stackcore::VideoSource> source =
            stackcore::open_video(_inputPath, [self currentOpenOptions]);
        _sourceFrames = source->frame_count();
        _sourceWidth = source->width();
        _sourceHeight = source->height();
        _byteOrderSuspect = source->byte_order_suspect() ? YES : NO;

        // 16bitと名乗っているのに実測が12bit幅に収まっていないか。
        // そのままだと画像が暗いだけで、破綻はしないので気づきにくい。
        const stackcore::FrameStats stats = source->frame_stats(0);
        _looksLikeShallowDepth =
            (source->bit_depth() >= 15 && stats.max_value > 0 && stats.max_value < 4096) ? YES : NO;

        [_frameSlider setMaxValue:std::max(0, _sourceFrames - 1)];
        [self refreshSelectionControl];
        [_frameSlider setDoubleValue:0.0];
        [self showSourceFrame:0];
        [_statusLabel setStringValue:
                          [NSString stringWithFormat:@"%@ — %@", [[item path] lastPathComponent],
                                                     [NSString stringWithUTF8String:
                                                                   source->describe().c_str()]]];
    } catch (const std::exception& e) {
        [_preview clearImage];
        [_statusLabel setStringValue:[NSString stringWithUTF8String:e.what()]];
    }
    [self updateBanner];

    [self loadSidecarForCurrent];
    [self updateApOverlay];
    [self updateControlsEnabled];
    [self updateNamePreview];
    [self updateDrizzleEstimate];
}

- (void)removeSelectedFromQueue:(id)sender {
    (void)sender;
    const NSInteger row = [_queueTable selectedRow];
    if (row < 0 || _running) return;
    [_items removeObjectAtIndex:static_cast<NSUInteger>(row)];
    _currentIndex = -1;
    [_queueTable reloadData];
    if ([_items count] > 0) {
        [self selectQueueIndex:std::min<NSInteger>(row, static_cast<NSInteger>([_items count]) - 1)];
    } else {
        _inputPath.clear();
        [_preview clearImage];
        [_preview clearAlignmentPoints];
        [_graph clearData];
    }
    [self updateControlsEnabled];
}

- (void)openDocument:(id)sender {
    (void)sender;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setAllowedFileTypes:@[ @"ser", @"avi" ]];
    [panel setAllowsMultipleSelection:YES];
    [panel setCanChooseDirectories:YES];
    if ([panel runModal] != NSModalResponseOK) return;
    NSMutableArray* paths = [NSMutableArray array];
    for (NSURL* url in [panel URLs]) [paths addObject:[url path]];
    [self addPathsToQueue:paths];
}

// ---- 設定の読み取りと無効化 -----------------------------------------------

// 入力の解釈（バイトオーダー・ビット深度）。
// **解析結果に効く。** 変えたら解析はやり直しになる。
- (stackcore::OpenOptions)currentOpenOptions {
    stackcore::OpenOptions options;
    const stackcore::ByteOrder orders[] = {stackcore::ByteOrder::Auto, stackcore::ByteOrder::Little,
                                           stackcore::ByteOrder::Big};
    options.endian = orders[[_endianPopup indexOfSelectedItem]];
    const int depths[] = {0, 12, 14};
    options.bit_depth_override = depths[[_depthPopup indexOfSelectedItem]];
    return options;
}

- (void)inputInterpretationChanged:(id)sender {
    (void)sender;
    // 読み方が変われば1枚目の見え方も変わる。開き直して確かめられるようにする。
    if (_currentIndex >= 0) [self selectQueueIndex:_currentIndex];
    [self updateControlsEnabled];
}

- (stackcore::MapStackSettings)currentSettings {
    stackcore::MapStackSettings settings;
    const stackcore::AlignMode modes[] = {stackcore::AlignMode::Auto, stackcore::AlignMode::Planet,
                                          stackcore::AlignMode::Lunar};
    settings.global.align.mode = modes[[_modeSegment selectedSegment]];
    settings.global.limit = _frameLimit;
    settings.global.quality_metric =
        [_qualityMetricPopup indexOfSelectedItem] == 1
            ? stackcore::QualityMetric::FrequencyBandPowerRatio
            : stackcore::QualityMetric::GradientEnergy;
    settings.reference_top_percent = [_topSlider doubleValue];
    settings.ap_top_percent = _selectionUsesCount ? _apTopPercentSetting
                                                  : [_apTopSlider doubleValue];
    settings.ap_top_count = _selectionUsesCount
                                ? static_cast<int>([_apTopSlider doubleValue] + 0.5)
                                : 0;
    settings.normalize_brightness = [_normalizeCheck state] == NSControlStateValueOn;
    const stackcore::StackMode stackModes[] = {stackcore::StackMode::Mean,
                                               stackcore::StackMode::QualityWeighted,
                                               stackcore::StackMode::SigmaClip};
    settings.stack_mode = stackModes[[_stackModePopup indexOfSelectedItem]];
    settings.reference_passes = ([_refineCheck state] == NSControlStateValueOn) ? 2 : 1;

    const int apSizes[] = {0, 32, 48, 64, 96, 128, 200};
    settings.ap.ap_size = apSizes[[_apSizePopup indexOfSelectedItem]];
    settings.ap.use_manual_points = _manualPointsActive ? true : false;
    settings.ap.manual_points = _manualPoints;
    settings.local.search_radius = static_cast<int>([_searchRadiusSlider doubleValue] + 0.5);

    const double scales[] = {1.0, 1.5, 2.0, 3.0};
    settings.drizzle_scale = scales[[_drizzleSegment selectedSegment]];
    settings.pixfrac = [_pixfracSlider doubleValue];
    return settings;
}

// 解析結果を使い回してよいかを判断するための指紋。
//
// **ここに入れる項目と入れない項目の区別が、このアプリの速さそのもの**である。
//   入れる  : 解析結果そのものが変わるもの（AP・参照・追跡）
//   入れない: 加算だけをやり直せば済むもの（AP別の選択率・Drizzle・低メモリ）
- (NSString*)analysisSignature {
    NSMutableString* s = [NSMutableString string];
    [s appendFormat:@"path=%@;", [self inputPathString]];
    [s appendFormat:@"method=%ld;", (long)[_methodPopup indexOfSelectedItem]];
    [s appendFormat:@"mode=%ld;", (long)[_modeSegment selectedSegment]];
    [s appendFormat:@"apsize=%ld;", (long)[_apSizePopup indexOfSelectedItem]];
    [s appendFormat:@"radius=%.0f;", [_searchRadiusSlider doubleValue]];
    [s appendFormat:@"reftop=%.2f;", [_topSlider doubleValue]];
    [s appendFormat:@"quality=%ld;", (long)[_qualityMetricPopup indexOfSelectedItem]];
    [s appendFormat:@"passes=%d;", ([_refineCheck state] == NSControlStateValueOn) ? 2 : 1];
    [s appendFormat:@"limit=%d;", _frameLimit];
    [s appendFormat:@"endian=%ld;depth=%ld;", (long)[_endianPopup indexOfSelectedItem],
                    (long)[_depthPopup indexOfSelectedItem]];
    [s appendFormat:@"manualOn=%d;manual=%zu:", _manualPointsActive ? 1 : 0,
                    _manualPoints.size()];
    for (std::size_t i = 0; i < _manualPoints.size(); ++i) {
        [s appendFormat:@"%d,%d;", _manualPoints[i].cx, _manualPoints[i].cy];
    }
    return s;
}

- (BOOL)analysisUsable {
    if (!_analysis || !_analysisSignature) return NO;
    return [_analysisSignature isEqualToString:[self analysisSignature]];
}

// 解析に影響する設定が変わった。
- (void)analysisSettingChanged:(id)sender {
    (void)sender;
    [self updateControlsEnabled];
    [self updateNamePreview];
}

- (void)topChanged:(id)sender {
    (void)sender;
    [_topValue setStringValue:[NSString stringWithFormat:@"%.0f %%", [_topSlider doubleValue]]];
    [_graph setCutPercent:[_topSlider doubleValue]];
    [self updateControlsEnabled];
}

- (void)apTopChanged:(id)sender {
    (void)sender;
    if (_selectionUsesCount) {
        _apTopCountSetting = static_cast<int>([_apTopSlider doubleValue] + 0.5);
        [_apTopValue setStringValue:
                         [NSString stringWithFormat:LSLocalizedString(@"%d 枚"),
                                                    _apTopCountSetting]];
    } else {
        _apTopPercentSetting = [_apTopSlider doubleValue];
        [_apTopValue setStringValue:[NSString stringWithFormat:@"%.0f %%", _apTopPercentSetting]];
    }
    [self updateNamePreview];
    [self updateControlsEnabled];
}

- (void)selectionModeChanged:(id)sender {
    (void)sender;
    if (_selectionUsesCount) {
        _apTopCountSetting = static_cast<int>([_apTopSlider doubleValue] + 0.5);
    } else {
        _apTopPercentSetting = [_apTopSlider doubleValue];
    }
    _selectionUsesCount = [_selectionModeSegment selectedSegment] == 1;
    [self refreshSelectionControl];
    [self updateNamePreview];
    [self updateControlsEnabled];
}

- (void)refreshSelectionControl {
    if (_selectionUsesCount) {
        const int available = _frameLimit > 0 ? std::min(_sourceFrames, _frameLimit) : _sourceFrames;
        // ファイルを開く前に枚数プリセットを選んでも値を1へ潰さない。
        const int maximum = available > 0 ? available : std::max(1, _apTopCountSetting);
        _apTopCountSetting = std::max(1, std::min(_apTopCountSetting, maximum));
        [_apTopCaption setStringValue:LSLocalizedString(@"AP別に採用するフレーム（枚数）")];
        [_apTopSlider setMinValue:1.0];
        [_apTopSlider setMaxValue:maximum];
        [_apTopSlider setDoubleValue:_apTopCountSetting];
    } else {
        [_apTopCaption setStringValue:LSLocalizedString(@"AP別に採用するフレーム（%）")];
        [_apTopSlider setMinValue:1.0];
        [_apTopSlider setMaxValue:100.0];
        [_apTopSlider setDoubleValue:_apTopPercentSetting];
    }
    [self apTopChanged:nil];
}

- (void)searchRadiusChanged:(id)sender {
    (void)sender;
    [_searchRadiusValue setStringValue:[NSString stringWithFormat:@"±%.0f",
                                                                 [_searchRadiusSlider doubleValue]]];
    [self updateControlsEnabled];
}

- (void)drizzleChanged:(id)sender {
    (void)sender;
    [_pixfracValue setStringValue:[NSString stringWithFormat:@"%.2f", [_pixfracSlider doubleValue]]];
    [self updateDrizzleEstimate];
    [self updateNamePreview];
}

- (void)updateDrizzleEstimate {
    const double scales[] = {1.0, 1.5, 2.0, 3.0};
    const double scale = scales[[_drizzleSegment selectedSegment]];
    if (scale <= 1.0 || _inputPath.empty()) {
        [_drizzleEstimate setStringValue:@""];
        return;
    }
    try {
        const std::unique_ptr<stackcore::VideoSource> source =
            stackcore::open_video(_inputPath, [self currentOpenOptions]);
        const int w = static_cast<int>(source->width() * scale);
        const int h = static_cast<int>(source->height() * scale);
        // 出力＋重みで float 2面ぶんを持つ。
        const double mb = static_cast<double>(w) * h * _sourceChannels * 4 * 2 / (1024.0 * 1024.0);
        [_drizzleEstimate
            setStringValue:[NSString
                               stringWithFormat:
                                   LSLocalizedString(@"出力 %d×%d ／ 作業メモリ約 %.0f MB"),
                               w, h, mb]];
    } catch (const std::exception&) {
        [_drizzleEstimate setStringValue:@""];
    }
}

- (void)graphModeChanged:(id)sender {
    (void)sender;
    [_graph setSortedByQuality:[_graphMode selectedSegment] == 1];
}

- (void)qualityGraphView:(QualityGraphView*)view didChangeCutPercent:(double)percent {
    (void)view;
    double p = percent;
    if (p < [_topSlider minValue]) p = [_topSlider minValue];
    if (p > [_topSlider maxValue]) p = [_topSlider maxValue];
    [_topSlider setDoubleValue:p];
    [_topValue setStringValue:[NSString stringWithFormat:@"%.0f %%", p]];
    [self updateControlsEnabled];
}

- (void)zoomChanged:(id)sender {
    (void)sender;
    const double zooms[] = {0.0, 1.0, 2.0, 4.0};
    [_preview setZoom:zooms[[_zoomControl selectedSegment]]];
}

- (void)apDisplayChanged:(id)sender {
    (void)sender;
    [_preview setShowAlignmentPoints:[_apShowCheck state] == NSControlStateValueOn];
    [_preview setApHeatmap:[_apHeatCheck state] == NSControlStateValueOn];
    [_preview setApEditing:[_apEditCheck state] == NSControlStateValueOn];
    if ([_apEditCheck state] == NSControlStateValueOn) {
        [[self window] makeFirstResponder:_preview];
    }
}

- (void)updateControlsEnabled {
    const BOOL hasFile = !_inputPath.empty();
    const BOOL globalOnly = [_methodPopup indexOfSelectedItem] == 1;
    const BOOL analysisOk = [self analysisUsable];

    // **実行中は選択を変えさせない。**
    // 変えると _inputPath が差し替わり、走り終わった仕事の解析結果が
    // 「今選んでいる別のファイル」のサイドカーとして書き出されてしまう。
    // しかも保存時にファイルサイズを今のファイルのもので上書きするので、
    // 枚数や大きさがたまたま同じ別撮りだと、次に開いたときの照合を
    // すり抜けて黙って誤った画像が出る。
    [_queueTable setEnabled:!_running];

    [_analyzeButton setEnabled:hasFile && !_running && !globalOnly];
    [_stackButton setEnabled:hasFile && !_running];
    [_batchButton setEnabled:([_items count] > 0) && !_running];
    [_cancelButton setEnabled:_running];
    [_saveButton setEnabled:(_displayed != nullptr) && !_running];

    [_apSizePopup setEnabled:!globalOnly];
    [_apTopSlider setEnabled:!globalOnly];
    [_selectionModeSegment setEnabled:!globalOnly];
    [_qualityMetricPopup setEnabled:!_running];
    [_stackModePopup setEnabled:!globalOnly && !_running];
    [_refineCheck setEnabled:!globalOnly];
    [_searchRadiusSlider setEnabled:!globalOnly];
    [_apEditCheck setEnabled:!globalOnly];

    // 「解析済み」であることを、押す前に分かるようにする（設計原則1.2）。
    [_stackButton setTitle:LSLocalizedString(analysisOk ? @"再スタック" : @"スタック")];
    if (_currentIndex >= 0 && !_running) {
        QueueItem* item = _items[static_cast<NSUInteger>(_currentIndex)];
        if ([item state] != QueueItemStateError && [item state] != QueueItemStateStacked) {
            [item setState:analysisOk ? QueueItemStateAnalyzed : QueueItemStatePending];
            [_queueTable reloadData];
            [_queueTable selectRowIndexes:[NSIndexSet indexSetWithIndex:
                                                          static_cast<NSUInteger>(_currentIndex)]
                     byExtendingSelection:NO];
        }
    }
}

// ---- 実行 -----------------------------------------------------------------

- (void)startRun {
    [self run:nil];
}

- (void)startAnalyzeOnly {
    [self analyze:nil];
}

- (void)startBatch {
    [self batch:nil];
}

- (void)setFrameLimit:(int)limit {
    _frameLimit = limit;
}

- (void)setBatchOutputDirectory:(NSString*)path {
    [_outputDirectory release];
    _outputDirectory = [path copy];
    [_outputDirLabel
        setStringValue:path ? path : LSLocalizedString(@"入力と同じフォルダ")];
}

- (void)analyze:(id)sender {
    (void)sender;
    [self beginJobWithStack:NO];
}

- (void)run:(id)sender {
    (void)sender;
    [self beginJobWithStack:YES];
}

- (void)cancel:(id)sender {
    (void)sender;
    _stopQueue->store(true);
    _cancelFlag->store(true);
    [_statusLabel setStringValue:LSLocalizedString(@"中断しています…")];
}

- (void)beginJobWithStack:(BOOL)wantStack {
    if (_running || _inputPath.empty()) return;

    _running = YES;
    _batchRunning = NO;
    _cancelFlag->store(false);
    _stopQueue->store(false);
    [self resetEta];
    [_progress setDoubleValue:0.0];
    [_progress setHidden:NO];
    [self updateControlsEnabled];

    // 設定はUIスレッドで読み取ってから値渡しする。
    // バックグラウンドからUIオブジェクトを触ってはいけない。
    JobRequest req;
    req.path = _inputPath;
    req.options = [self currentOpenOptions];
    req.settings = [self currentSettings];
    req.global_only = [_methodPopup indexOfSelectedItem] == 1;
    req.low_memory = [_lowMemoryCheck state] == NSControlStateValueOn;
    req.want_stack = wantStack ? true : false;
    if ([self analysisUsable]) req.reuse = _analysis;

    NSString* signature = [[self analysisSignature] copy];
    const BOOL reusing = (req.reuse != nullptr);
    [_statusLabel
        setStringValue:LSLocalizedString(reusing ? @"解析結果を使い回して加算します…"
                                                 : @"開始しています…")];

    std::atomic<bool>* cancelFlag = _cancelFlag;
    MainWindowController* controller = self;

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        const stackcore::ProgressFn progress =
            [controller, cancelFlag](const char* stage, int done, int total) -> bool {
            if (cancelFlag->load()) return false;
            // 毎フレーム投げると描画で溢れるので、1%刻みに間引く。
            const int step = total < 100 ? 1 : total / 100;
            if (done % step == 0 || done == total) {
                NSString* text = [NSString stringWithUTF8String:stage];
                dispatch_async(dispatch_get_main_queue(), ^{
                    [controller reportStage:text done:done total:total];
                });
            }
            return true;
        };

        JobResult result = run_job(req, progress);
        dispatch_async(dispatch_get_main_queue(), ^{
            [controller finishJob:result signature:signature wantedStack:req.want_stack];
            [signature release];
        });
    });
}

- (void)finishJob:(const JobResult&)result
        signature:(NSString*)signature
      wantedStack:(bool)wantedStack {
    _running = NO;
    [_progress setDoubleValue:0.0];
    [_progress setHidden:YES];
    [self resetEta];

    if (result.cancelled) {
        [_statusLabel setStringValue:LSLocalizedString(@"中断しました")];
        [self updateControlsEnabled];
        if (_onRunFinished) _onRunFinished();
        return;
    }
    if (!result.error.empty()) {
        [_statusLabel setStringValue:LSLocalizedString(@"失敗しました")];
        if (_currentIndex >= 0) {
            QueueItem* item = _items[static_cast<NSUInteger>(_currentIndex)];
            [item setState:QueueItemStateError];
            [item setMessage:[NSString stringWithUTF8String:result.error.c_str()]];
            [_queueTable reloadData];
        }
        [self showError:[NSString stringWithUTF8String:result.error.c_str()]
                  title:LSLocalizedString(@"処理できませんでした")];
        [self updateControlsEnabled];
        if (_onRunFinished) _onRunFinished();
        return;
    }

    if (result.analysis) {
        _analysis = result.analysis;
        [_analysisSignature release];
        _analysisSignature = [signature copy];
        if (!result.reused_analysis) [self saveSidecarForCurrent];
        [self rebuildReferenceImage];
    }
    [self showFrames:result.frames];

    if (wantedStack && result.image) {
        _stacked = result.image;
        // 分解はここで1回だけ行う。以降スライダーを動かしても再構成しか走らない。
        _wavelet = std::make_shared<stackcore::WaveletSharpener>();
        _wavelet->analyze(*_stacked, kWaveletLayers);
        [_viewModeSegment setSelectedSegment:2];
        // 結果が出たら次の工程（仕上げ・書き出し）のタブへ進める。
        // 「スタックし終わったのに次に何をするのか分からない」を防ぐ。
        [_inspectorTab setSelectedSegment:1];
        [self updateInspectorVisibility];
        [self applyWavelet];
        [_statusLabel setStringValue:[NSString stringWithFormat:LSLocalizedString(@"完了 — %d×%d"),
                                                                _stacked->width(),
                                                                _stacked->height()]];
        [self notifyDone:[NSString stringWithFormat:LSLocalizedString(@"スタックが完了しました（%d×%d）"),
                                                    _stacked->width(), _stacked->height()]];
    } else {
        [_statusLabel setStringValue:
                          [NSString stringWithFormat:
                                        LSLocalizedString(@"解析が終わりました — AP %d個 / %d フレーム"),
                                                     _analysis ? static_cast<int>(
                                                                     _analysis->points.size())
                                                               : 0,
                                                     static_cast<int>(result.frames.size())]];
        if (_referenceImage) {
            [_viewModeSegment setSelectedSegment:1];
            [_preview showFrameBuffer:*_referenceImage];
        }
        [self notifyDone:LSLocalizedString(@"解析が完了しました")];
    }
    // **APオーバーレイの更新は _stacked を入れたあとに行う。**
    // オーバーレイの座標倍率はDrizzle倍率から決まるが、その判断に
    // 「スタック結果があるか」を使っている。順番を逆にすると、
    // 2倍で出した画像の上に等倍のAP枠が描かれ、左上の1/4に縮んで並ぶ。
    [self updateApOverlay];
    [self updateControlsEnabled];
    if (_onRunFinished) _onRunFinished();
}

// サイドカーに入っている参照画像を FrameBuffer に戻す。
// 「解析で何を基準にしたか」は、結果が変なときに真っ先に見たいものである。
- (void)rebuildReferenceImage {
    _referenceImage.reset();
    if (!_analysis) return;
    const int w = _analysis->width, h = _analysis->height, c = _analysis->channels;
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    if (w <= 0 || h <= 0 || c <= 0 ||
        _analysis->reference.size() < pixels * static_cast<std::size_t>(c)) {
        return;
    }
    auto image = std::make_shared<stackcore::FrameBuffer>();
    image->reset(w, h, c);
    for (int ch = 0; ch < c; ++ch) {
        for (int y = 0; y < h; ++y) {
            const float* src = _analysis->reference.data() +
                               static_cast<std::size_t>(ch) * pixels +
                               static_cast<std::size_t>(y) * w;
            float* dst = image->row(ch, y);
            for (int x = 0; x < w; ++x) dst[x] = src[x];
        }
    }
    image->invalidate_luma();
    _referenceImage = image;
}

- (void)showFrames:(const std::vector<stackcore::FrameInfo>&)frames {
    if (frames.empty()) {
        [_graph clearData];
        return;
    }
    std::vector<double> q(frames.size());
    std::vector<unsigned char> ok(frames.size());
    int rejected = 0;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        q[i] = frames[i].quality;
        ok[i] = frames[i].accepted ? 1 : 0;
        if (!frames[i].accepted) ++rejected;
    }
    [_graph setQualities:q.data() accepted:ok.data() count:static_cast<int>(frames.size())];
    [_graph setCutPercent:[_topSlider doubleValue]];

    _rejectedFrames = rejected;
    [self updateBanner];
}

// 警告バナー（UI設計書 §7.2）。
//
// CLIの `info` が出す診断と同じことを、GUIでも見逃さないようにする。
// **黙って処理しない**のが趣旨なので、原因と対処先を1行にまとめる。
- (void)updateBanner {
    NSMutableArray* parts = [NSMutableArray array];
    if (_byteOrderSuspect) {
        [parts addObject:LSLocalizedString(
                             @"バイトオーダーがヘッダの主張と違います。画像が破綻して見えるならStackで切り替えてください")];
    }
    if (_looksLikeShallowDepth) {
        [parts addObject:LSLocalizedString(
                             @"16bitですが実測は12bit幅です。暗く写るならStackで「12bitとして扱う」を選んでください")];
    }
    if (_rejectedFrames > 0) {
        [parts addObject:[NSString
                             stringWithFormat:
                                 LSLocalizedString(@"%d 枚を自動除外しました（視野外・追跡失敗）"),
                                                    _rejectedFrames]];
    }
    [_bannerLabel setStringValue:[parts componentsJoinedByString:@" ／ "]];
}

// 入力の1枚を表示する。
- (void)showSourceFrame:(int)index {
    if (_inputPath.empty()) return;
    try {
        const std::unique_ptr<stackcore::VideoSource> source =
            stackcore::open_video(_inputPath, [self currentOpenOptions]);
        if (index < 0) index = 0;
        if (index >= source->frame_count()) index = source->frame_count() - 1;
        stackcore::FrameBuffer cfa, rgb;
        const stackcore::FrameBuffer* frame =
            stackcore::read_prepared_frame(*source, index, false, cfa, rgb);
        _sourceChannels = frame->channels();
        [_preview showFrameBuffer:*frame];
    } catch (const std::exception&) {
        [_preview clearImage];
    }
}

- (void)frameSliderChanged:(id)sender {
    (void)sender;
    [_viewModeSegment setSelectedSegment:0];
    [self showSourceFrame:static_cast<int>([_frameSlider doubleValue] + 0.5)];
}

// 表示モードの切替（UI設計書 §5.1）。
- (void)viewModeChanged:(id)sender {
    (void)sender;
    switch ([_viewModeSegment selectedSegment]) {
        case 1:
            if (_referenceImage) {
                [_preview showFrameBuffer:*_referenceImage];
            } else {
                [_statusLabel
                    setStringValue:LSLocalizedString(@"参照画像はまだありません（解析すると作られます）")];
                [_viewModeSegment setSelectedSegment:0];
            }
            break;
        case 2:
            if (_displayed) {
                [_preview showFrameBuffer:*_displayed];
            } else {
                [_statusLabel setStringValue:LSLocalizedString(@"スタック結果はまだありません")];
                [_viewModeSegment setSelectedSegment:0];
            }
            break;
        case 0:
        default:
            [self showSourceFrame:static_cast<int>([_frameSlider doubleValue] + 0.5)];
            break;
    }
}

- (void)updateApOverlay {
    if (!_analysis || _analysis->points.empty()) {
        [_preview clearAlignmentPoints];
        [_apCountLabel setStringValue:_manualPoints.empty()
                                          ? @""
                                          : [NSString stringWithFormat:
                                                            LSLocalizedString(@"手動AP %zu 個（未解析）"),
                                                                       _manualPoints.size()]];
        return;
    }
    const double scales[] = {1.0, 1.5, 2.0, 3.0};
    const double scale = _stacked ? scales[[_drizzleSegment selectedSegment]] : 1.0;
    const std::vector<double> quality = ap_mean_quality(*_analysis);
    [_preview setAlignmentPoints:_analysis->points
                          apSize:_analysis->ap_size
                   meanQualities:quality
                 coordinateScale:scale];
    [_apCountLabel setStringValue:[NSString stringWithFormat:LSLocalizedString(@"AP %zu 個 / %d px"),
                                                             _analysis->points.size(),
                                                             _analysis->ap_size]];
}

// ---- 進捗と残り時間 --------------------------------------------------------

- (void)resetEta {
    [_etaStage release];
    _etaStage = nil;
    _etaStart = 0.0;
}

- (void)reportStage:(NSString*)stage done:(int)done total:(int)total {
    stage = LSLocalizedString(stage);
    const NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
    if (!_etaStage || ![_etaStage isEqualToString:stage]) {
        // フェーズが変わったら測り直す。
        // 品質評価とスタックでは1フレームあたりの重さが桁で違うので、
        // 通しで平均すると残り時間がまるで当たらない。
        [_etaStage release];
        _etaStage = [stage copy];
        _etaStart = now;
    }

    const double fraction = total > 0 ? static_cast<double>(done) / total : 0.0;
    [_progress setHidden:NO];
    [_progress setDoubleValue:fraction];

    NSString* eta = @"";
    const NSTimeInterval elapsed = now - _etaStart;
    if (fraction > 0.02 && elapsed > 1.0) {
        const double remain = elapsed * (1.0 - fraction) / fraction;
        eta = [NSString stringWithFormat:LSLocalizedString(@"　残り約 %@"),
                                         [self formatSeconds:remain]];
    }
    [_statusLabel setStringValue:[NSString stringWithFormat:@"%@  %d / %d（%.0f%%）%@", stage, done,
                                                            total, fraction * 100.0, eta]];
}

- (NSString*)formatSeconds:(double)seconds {
    if (seconds < 60.0)
        return [NSString stringWithFormat:LSLocalizedString(@"%.0f秒"), seconds];
    const int m = static_cast<int>(seconds / 60.0);
    const int s = static_cast<int>(seconds - m * 60.0);
    if (m < 60)
        return [NSString stringWithFormat:LSLocalizedString(@"%d分%02d秒"), m, s];
    return [NSString stringWithFormat:LSLocalizedString(@"%d時間%d分"), m / 60, m % 60];
}

- (void)notifyDone:(NSString*)text {
    // 長い処理では席を外しているので、終わったら知らせる（UI設計書 §1.3）。
    //
    // UNUserNotificationCenter は10.14以降。10.13を切れないので
    // NSUserNotification を使う。新しいSDKでは非推奨警告が出るが、
    // 「10.13で動くこと」を優先して黙らせている。
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSUserNotification* note = [[[NSUserNotification alloc] init] autorelease];
    [note setTitle:@"LunaStack"];
    [note setInformativeText:text];
    [[NSUserNotificationCenter defaultUserNotificationCenter] deliverNotification:note];
#pragma clang diagnostic pop
}

// ---- サイドカー -----------------------------------------------------------

// 入力パスを NSString で得る。
// パスに日本語が入りうるので、常に UTF-8 として変換する。
- (NSString*)inputPathString {
    return [NSString stringWithUTF8String:_inputPath.c_str()];
}

- (NSString*)sidecarPath {
    // **`%s` を使ってはいけない。** NSString の書式指定子 `%s` は
    // UTF-8ではなくシステムのCエンコーディングで解釈するため、
    // 日本語を含むパスが化けて、書き込みが黙って失敗する。
    // 実際、これで「サイドカーが作られない」不具合になっていた。
    return [[self inputPathString] stringByAppendingPathExtension:@"lstk"];
}

// 解析に使った設定を別ファイルで持つ。
//
// サイドカー本体（.lstk）はエンジンの形式で、参照フレームやAP変位場は入るが
// 「どの設定で解析したか」は入らない。設定が分からないまま読み込むと、
// 画面のつまみと中身が食い違ったまま再スタックできてしまう。
- (NSString*)sidecarSettingsPath {
    return [[self sidecarPath] stringByAppendingPathExtension:@"json"];
}

- (void)saveSidecarForCurrent {
    if (!_analysis) return;

    // ファイルサイズはエンジン側では埋まらない（解析器はファイルではなく
    // VideoSource しか知らない）。ここで入れておかないと、次に開いたときの
    // 照合が「サイズ 0 と食い違う」で必ず落ちる。
    NSDictionary* attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:
                                                              [self inputPathString]
                                                                          error:NULL];
    _analysis->source_size = [attrs[NSFileSize] longLongValue];

    try {
        stackcore::save_sidecar(std::string([[self sidecarPath] UTF8String]), *_analysis);
    } catch (const std::exception& e) {
        NSLog(@"サイドカーを保存できませんでした: %@", [NSString stringWithUTF8String:e.what()]);
        return;
    }
    NSDictionary* meta = @{
        @"signature" : [self analysisSignature],
        @"settings" : [self settingsDictionary]
    };
    NSData* data = [NSJSONSerialization dataWithJSONObject:meta options:0 error:NULL];
    [data writeToFile:[self sidecarSettingsPath] atomically:YES];
}

- (void)loadSidecarForCurrent {
    NSFileManager* fm = [NSFileManager defaultManager];
    if (![fm fileExistsAtPath:[self sidecarPath]] ||
        ![fm fileExistsAtPath:[self sidecarSettingsPath]]) {
        return;
    }

    NSData* data = [NSData dataWithContentsOfFile:[self sidecarSettingsPath]];
    id meta = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL] : nil;
    if (![meta isKindOfClass:[NSDictionary class]]) return;
    NSDictionary* settings = meta[@"settings"];
    if (![settings isKindOfClass:[NSDictionary class]]) return;

    auto loaded = std::make_shared<stackcore::AnalysisData>();
    try {
        stackcore::load_sidecar(std::string([[self sidecarPath] UTF8String]), *loaded);
    } catch (const std::exception& e) {
        NSLog(@"サイドカーを読めませんでした: %@", [NSString stringWithUTF8String:e.what()]);
        return;
    }

    // 入力が変わっていないことを確かめる。
    // 古い解析結果を別の中身に当てると、黙って誤った画像が出る。
    NSDictionary* attrs = [fm attributesOfItemAtPath:
                                  [NSString stringWithUTF8String:_inputPath.c_str()]
                                               error:NULL];
    const long long size = [attrs[NSFileSize] longLongValue];
    std::string message;
    if (!stackcore::matches_source(*loaded, size, _sourceFrames, _sourceWidth, _sourceHeight,
                                   _sourceChannels, message)) {
        NSLog(@"サイドカーが入力と合いません: %@", [NSString stringWithUTF8String:message.c_str()]);
        return;
    }

    // 解析時の設定を画面に戻してから、指紋を照合する。
    // 仕上げのつまみは戻さない（解析とは無関係で、触っていた値を奪わない）。
    [self applySettingsDictionary:settings includePostProcessing:NO];
    _analysis = loaded;
    [_analysisSignature release];
    _analysisSignature = [[self analysisSignature] copy];

    NSString* recorded = meta[@"signature"];
    if ([recorded isKindOfClass:[NSString class]] &&
        ![recorded isEqualToString:_analysisSignature]) {
        // 設定を戻しても一致しないなら、信用せずに捨てる。
        _analysis.reset();
        [_analysisSignature release];
        _analysisSignature = nil;
        return;
    }

    [self rebuildReferenceImage];
    [self showFrames:_analysis->frames];
    [_statusLabel
        setStringValue:LSLocalizedString(@"解析済みの結果を読み込みました。すぐに再スタックできます")];
}

// ---- プリセット -----------------------------------------------------------

// プリセットとサイドカーの両方で使う、設定一式の辞書表現。
- (NSDictionary*)settingsDictionary {
    NSMutableArray* sharpen = [NSMutableArray array];
    NSMutableArray* denoise = [NSMutableArray array];
    for (int j = 0; j < kWaveletLayers; ++j) {
        [sharpen addObject:@([_sharpenSliders[j] doubleValue])];
        [denoise addObject:@([_denoiseSliders[j] doubleValue])];
    }
    return @{
        @"method" : @([_methodPopup indexOfSelectedItem]),
        @"mode" : @([_modeSegment selectedSegment]),
        @"apSize" : @([_apSizePopup indexOfSelectedItem]),
        @"searchRadius" : @([_searchRadiusSlider doubleValue]),
        @"qualityMetric" : @([_qualityMetricPopup indexOfSelectedItem]),
        @"referenceTopPercent" : @([_topSlider doubleValue]),
        @"refine" : @([_refineCheck state] == NSControlStateValueOn),
        @"selectionMode" : @(_selectionUsesCount ? 1 : 0),
        @"apTopPercent" : @(_apTopPercentSetting),
        @"apTopCount" : @(_apTopCountSetting),
        @"normalizeBrightness" : @([_normalizeCheck state] == NSControlStateValueOn),
        @"stackMode" : @([_stackModePopup indexOfSelectedItem]),
        @"lowMemory" : @([_lowMemoryCheck state] == NSControlStateValueOn),
        @"drizzle" : @([_drizzleSegment selectedSegment]),
        @"pixfrac" : @([_pixfracSlider doubleValue]),
        @"sharpen" : sharpen,
        @"denoise" : denoise,
        @"format" : @([_formatPopup indexOfSelectedItem]),
        @"endian" : @([_endianPopup indexOfSelectedItem]),
        @"bitDepth" : @([_depthPopup indexOfSelectedItem]),
    };
}

- (void)applySettingsDictionary:(NSDictionary*)d {
    [self applySettingsDictionary:d includePostProcessing:YES];
}

// includePostProcessing が NO のとき、ウェーブレットのつまみは触らない。
//
// サイドカーの読み込みで使う。解析結果に付いてきた設定を戻すのは
// 「画面と中身を一致させる」ためであって、**仕上げは解析と無関係**である。
// 触っていた仕上げが、ファイルを開き直しただけで勝手に動くのは
// 「後処理は非破壊」（UI設計書 §1.5）に反する。
- (void)applySettingsDictionary:(NSDictionary*)d includePostProcessing:(BOOL)includePost {
    if (d[@"method"]) [_methodPopup selectItemAtIndex:[d[@"method"] integerValue]];
    if (d[@"mode"]) [_modeSegment setSelectedSegment:[d[@"mode"] integerValue]];
    if (d[@"apSize"]) [_apSizePopup selectItemAtIndex:[d[@"apSize"] integerValue]];
    if (d[@"searchRadius"]) [_searchRadiusSlider setDoubleValue:[d[@"searchRadius"] doubleValue]];
    if (d[@"qualityMetric"]) {
        [_qualityMetricPopup selectItemAtIndex:[d[@"qualityMetric"] integerValue]];
    }
    if (d[@"referenceTopPercent"]) {
        [_topSlider setDoubleValue:[d[@"referenceTopPercent"] doubleValue]];
    }
    if (d[@"refine"]) {
        [_refineCheck setState:[d[@"refine"] boolValue] ? NSControlStateValueOn
                                                        : NSControlStateValueOff];
    }
    if (d[@"apTopPercent"]) _apTopPercentSetting = [d[@"apTopPercent"] doubleValue];
    if (d[@"apTopCount"]) _apTopCountSetting = [d[@"apTopCount"] intValue];
    _selectionUsesCount = d[@"selectionMode"] && [d[@"selectionMode"] integerValue] == 1;
    [_selectionModeSegment setSelectedSegment:_selectionUsesCount ? 1 : 0];
    [self refreshSelectionControl];
    if (d[@"normalizeBrightness"]) {
        [_normalizeCheck setState:[d[@"normalizeBrightness"] boolValue]
                                      ? NSControlStateValueOn
                                      : NSControlStateValueOff];
    }
    if (d[@"stackMode"]) [_stackModePopup selectItemAtIndex:[d[@"stackMode"] integerValue]];
    if (d[@"lowMemory"]) {
        [_lowMemoryCheck setState:[d[@"lowMemory"] boolValue] ? NSControlStateValueOn
                                                              : NSControlStateValueOff];
    }
    if (d[@"drizzle"]) [_drizzleSegment setSelectedSegment:[d[@"drizzle"] integerValue]];
    if (d[@"pixfrac"]) [_pixfracSlider setDoubleValue:[d[@"pixfrac"] doubleValue]];
    if (d[@"format"]) [_formatPopup selectItemAtIndex:[d[@"format"] integerValue]];
    if (d[@"endian"]) [_endianPopup selectItemAtIndex:[d[@"endian"] integerValue]];
    if (d[@"bitDepth"]) [_depthPopup selectItemAtIndex:[d[@"bitDepth"] integerValue]];

    NSArray* sharpen = includePost ? d[@"sharpen"] : nil;
    NSArray* denoise = includePost ? d[@"denoise"] : nil;
    for (int j = 0; j < kWaveletLayers; ++j) {
        if ([sharpen isKindOfClass:[NSArray class]] &&
            static_cast<NSUInteger>(j) < [sharpen count]) {
            [_sharpenSliders[j] setDoubleValue:[sharpen[static_cast<NSUInteger>(j)] doubleValue]];
        }
        if ([denoise isKindOfClass:[NSArray class]] &&
            static_cast<NSUInteger>(j) < [denoise count]) {
            [_denoiseSliders[j] setDoubleValue:[denoise[static_cast<NSUInteger>(j)] doubleValue]];
        }
    }

    [self topChanged:nil];
    [self refreshSelectionControl];
    [self searchRadiusChanged:nil];
    [self drizzleChanged:nil];
    [self waveletChanged:nil];
}

- (void)reloadPresets {
    [_presetPopup removeAllItems];
    [_presetPopup addItemWithTitle:LSLocalizedString(@"プリセット…")];
    for (NSString* name in [Presets names]) [_presetPopup addItemWithTitle:name];
}

- (void)presetSelected:(id)sender {
    (void)sender;
    if ([_presetPopup indexOfSelectedItem] <= 0) return;
    NSDictionary* d = [Presets loadSettingsNamed:[_presetPopup titleOfSelectedItem]];
    if (!d) {
        [self showError:LSLocalizedString(@"プリセットを読めませんでした")
                  title:LSLocalizedString(@"プリセット")];
        return;
    }
    [self applySettingsDictionary:d];
    [self updateControlsEnabled];
}

- (void)savePreset:(id)sender {
    (void)sender;
    NSAlert* alert = [[[NSAlert alloc] init] autorelease];
    [alert setMessageText:LSLocalizedString(@"プリセットの名前")];
    [alert addButtonWithTitle:LSLocalizedString(@"保存")];
    [alert addButtonWithTitle:LSLocalizedString(@"やめる")];
    NSTextField* field =
        [[[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 240, 24)] autorelease];
    [field setStringValue:LSLocalizedString(@"マイ設定")];
    [alert setAccessoryView:field];
    if ([alert runModal] != NSAlertFirstButtonReturn) return;

    NSString* error = nil;
    if (![Presets saveSettings:[self settingsDictionary]
                          name:[field stringValue]
                         error:&error]) {
        [self showError:error ? error : LSLocalizedString(@"保存できませんでした")
                  title:LSLocalizedString(@"プリセット")];
        return;
    }
    [self reloadPresets];
    [_statusLabel setStringValue:[NSString
                                     stringWithFormat:
                                         LSLocalizedString(@"プリセット「%@」を保存しました"),
                                                            [field stringValue]]];
}

// APの当たり判定を自己検証する。
//
// **描画の座標変換と当たり判定の座標変換がずれる**のが、この手のUIで
// いちばん気づきにくい壊れ方である。枠は正しく見えているのに
// クリックすると別のAPが選ばれる、という形で出る。
// 画面に描いた位置をそのまま押したとき、同じAPが返るかを全点で確かめる。
- (BOOL)selfCheckApHitTest {
    if (!_analysis || _analysis->points.empty()) {
        NSLog(@"AP当たり判定の自己検証: APが無いので確認できません");
        return NO;
    }
    const double scales[] = {1.0, 1.5, 2.0, 3.0};
    const double coordScale = _stacked ? scales[[_drizzleSegment selectedSegment]] : 1.0;

    int checked = 0, mismatched = 0;
    double maxRoundTrip = 0.0;
    for (std::size_t i = 0; i < _analysis->points.size(); ++i) {
        const NSPoint img = NSMakePoint(_analysis->points[i].cx * coordScale,
                                        _analysis->points[i].cy * coordScale);
        const NSPoint view = [_preview viewPointFromImagePoint:img];
        const NSPoint back = [_preview imagePointFromViewPoint:view];
        maxRoundTrip = std::max(maxRoundTrip,
                                std::max(std::fabs(back.x - img.x), std::fabs(back.y - img.y)));

        const NSInteger hit = [_preview apIndexAtViewPoint:view];
        if (hit != static_cast<NSInteger>(i)) ++mismatched;
        ++checked;
    }
    NSLog(@"AP当たり判定の自己検証: %d点中 一致しない %d点 / 往復誤差 最大 %.4f px", checked,
          mismatched, maxRoundTrip);
    return mismatched == 0 && maxRoundTrip < 0.01;
}

// AP編集とプリセットの往復を自己検証する。
//
// どちらも「画面のつまみ → 設定 → 保存 → 復元」の受け渡しであり、
// 途中の1項目を書き忘れても画面上は何も起きない。気づくのは
// 「プリセットを読んだのに前と結果が違う」という遠い場所になる。
- (BOOL)selfCheckEditingAndPresets {
    BOOL ok = YES;

    // --- AP編集 ---
    if (_analysis && !_analysis->points.empty()) {
        const std::size_t before = _analysis->points.size();
        NSString* signatureBefore = [[self analysisSignature] copy];

        [self previewView:_preview didDeleteApAtIndex:0];
        [self previewView:_preview didAddApAtX:_analysis ? 0 : 10 y:10];

        const stackcore::MapStackSettings settings = [self currentSettings];
        if (!settings.ap.use_manual_points) {
            NSLog(@"AP編集の自己検証: 手動配置が有効になっていません");
            ok = NO;
        }
        if (settings.ap.manual_points.size() != before) {
            // 1つ消して1つ足したので数は変わらない
            NSLog(@"AP編集の自己検証: AP数が合いません（%zu → %zu、期待 %zu）", before,
                  settings.ap.manual_points.size(), before);
            ok = NO;
        }
        if ([signatureBefore isEqualToString:[self analysisSignature]]) {
            NSLog(@"AP編集の自己検証: APを変えたのに解析が無効になっていません");
            ok = NO;
        }
        [signatureBefore release];

        // 元に戻す
        _manualPointsActive = NO;
        _manualPoints.clear();
    }

    // --- プリセット ---
    NSDictionary* original = [self settingsDictionary];
    [_apTopSlider setDoubleValue:37.0];
    [_topSlider setDoubleValue:41.0];
    [_sharpenSliders[2] setDoubleValue:2.25];
    [_denoiseSliders[4] setDoubleValue:0.55];
    [_drizzleSegment setSelectedSegment:2];
    [_endianPopup selectItemAtIndex:2];
    NSDictionary* modified = [self settingsDictionary];

    NSString* error = nil;
    NSString* name = @"__self_check__";
    if (![Presets saveSettings:modified name:name error:&error]) {
        NSLog(@"プリセットの自己検証: 保存に失敗 %@", error);
        ok = NO;
    } else {
        [self applySettingsDictionary:original];
        NSDictionary* loaded = [Presets loadSettingsNamed:name];
        [self applySettingsDictionary:loaded];
        NSDictionary* restored = [self settingsDictionary];
        for (NSString* key in modified) {
            if (![[restored[key] description] isEqualToString:[modified[key] description]]) {
                NSLog(@"プリセットの自己検証: %@ が戻りません（%@ → %@）", key, modified[key],
                      restored[key]);
                ok = NO;
            }
        }
        [Presets removeSettingsNamed:name];
        [self applySettingsDictionary:original];
    }

    NSLog(@"AP編集・プリセットの自己検証: %@", ok ? @"問題なし" : @"問題あり");
    return ok;
}

// ---- APの手動編集 ---------------------------------------------------------

- (void)previewView:(PreviewView*)view didAddApAtX:(int)x y:(int)y {
    (void)view;
    [self ensureManualPointsInitialized];
    stackcore::AlignmentPoint p;
    p.cx = x;
    p.cy = y;
    _manualPoints.push_back(p);
    [self applyManualPoints];
}

- (void)previewView:(PreviewView*)view didDeleteApAtIndex:(NSInteger)index {
    (void)view;
    [self ensureManualPointsInitialized];
    if (index < 0 || index >= static_cast<NSInteger>(_manualPoints.size())) return;
    _manualPoints.erase(_manualPoints.begin() + index);
    [self applyManualPoints];
}

// 自動配置の結果を手動リストの初期値にする。
// 空のまま1点追加すると「AP1個だけ」になり、その挙動は誰も望んでいない。
- (void)ensureManualPointsInitialized {
    if (_manualPointsActive) return;
    _manualPointsActive = YES;
    if (_analysis && !_analysis->points.empty()) _manualPoints = _analysis->points;
}

- (void)applyManualPoints {
    // AP集合が変わったら解析結果はもう使えない。
    _analysis.reset();
    [_analysisSignature release];
    _analysisSignature = nil;

    const std::vector<double> none;
    const int apSizes[] = {0, 32, 48, 64, 96, 128, 200};
    int size = apSizes[[_apSizePopup indexOfSelectedItem]];
    if (size == 0) size = 64;  // 自動のときは表示だけ暫定値で描く
    [_preview setAlignmentPoints:_manualPoints apSize:size meanQualities:none coordinateScale:1.0];
    [_apCountLabel setStringValue:[NSString stringWithFormat:
                                                        LSLocalizedString(@"手動AP %zu 個（未解析）"),
                                                             _manualPoints.size()]];
    [self updateControlsEnabled];
}

- (void)resetApPlacement:(id)sender {
    (void)sender;
    _manualPointsActive = NO;
    _manualPoints.clear();
    _analysis.reset();
    [_analysisSignature release];
    _analysisSignature = nil;
    [_preview clearAlignmentPoints];
    [_apCountLabel setStringValue:@""];
    [self updateControlsEnabled];
}

- (void)clearApPlacement:(id)sender {
    (void)sender;
    // 「自動配置に戻す」とは別の操作。消したまま解析すれば
    // 「APが1つも置けませんでした」で止まる。それが正しい。
    // 消したのに黙って置き直されるほうが、よほど分かりにくい。
    _manualPointsActive = YES;
    _manualPoints.clear();
    _analysis.reset();
    [_analysisSignature release];
    _analysisSignature = nil;
    [_preview clearAlignmentPoints];
    [_apCountLabel setStringValue:LSLocalizedString(@"AP 0 個（手動）")];
    [_statusLabel
        setStringValue:LSLocalizedString(@"APをすべて消しました。プレビューをクリックして置き直せます")];
    [self updateControlsEnabled];
}

// ---- 後処理 ---------------------------------------------------------------

- (void)stretchToggled:(id)sender {
    [_preview setDisplayStretch:[(NSButton*)sender state] == NSControlStateValueOn];
}

- (void)waveletChanged:(id)sender {
    (void)sender;
    for (int j = 0; j < kWaveletLayers; ++j) {
        [_sharpenValues[j] setStringValue:[NSString stringWithFormat:@"%.2f",
                                                                    [_sharpenSliders[j]
                                                                        doubleValue]]];
        [_denoiseValues[j] setStringValue:[NSString stringWithFormat:@"%.2f",
                                                                    [_denoiseSliders[j]
                                                                        doubleValue]]];
    }
    [self applyWavelet];
}

- (void)resetWavelet:(id)sender {
    (void)sender;
    for (int j = 0; j < kWaveletLayers; ++j) {
        [_sharpenSliders[j] setDoubleValue:1.0];
        [_denoiseSliders[j] setDoubleValue:0.0];
    }
    [self waveletChanged:nil];
}

- (void)applyWavelet {
    if (!_wavelet || !_wavelet->ready()) return;

    std::vector<stackcore::WaveletLayerParams> params(kWaveletLayers);
    for (int j = 0; j < kWaveletLayers; ++j) {
        params[static_cast<std::size_t>(j)].sharpen = [_sharpenSliders[j] doubleValue];
        params[static_cast<std::size_t>(j)].denoise = [_denoiseSliders[j] doubleValue];
    }

    auto out = std::make_shared<stackcore::FrameBuffer>();
    _wavelet->synthesize(params, *out);
    out->set_source_bit_depth(_stacked->source_bit_depth());
    _displayed = out;
    [_preview showFrameBuffer:*_displayed];
}

- (void)setDrizzleIndexForTesting:(int)index {
    if (index < 0 || index > 3) return;
    [_drizzleSegment setSelectedSegment:index];
    [self drizzleChanged:nil];
}

- (void)setZoomIndexForTesting:(int)index {
    if (index < 0 || index > 3) return;
    [_zoomControl setSelectedSegment:index];
    [self zoomChanged:nil];
}

- (void)setApHeatmapForTesting:(BOOL)on {
    [_apHeatCheck setState:on ? NSControlStateValueOn : NSControlStateValueOff];
    [self apDisplayChanged:nil];
}

- (void)setSharpenForTesting:(double)value denoise:(double)denoise {
    for (int j = 0; j < kWaveletLayers; ++j) {
        [_sharpenSliders[j] setDoubleValue:(j < 3 ? value : 1.0)];
        [_denoiseSliders[j] setDoubleValue:denoise];
    }
    [self waveletChanged:nil];
}

// ---- 書き出し -------------------------------------------------------------

- (OutputFormat)currentOutputFormat {
    const NSInteger index = [_formatPopup indexOfSelectedItem];
    if (index == 1) return OutputFormat::TiffFloat32;
    if (index == 2) return OutputFormat::Png16;
    return OutputFormat::Tiff16;
}

// UI設計書 §4.6 の命名規則 `{元名}_ap{AP}_{率}pct_x{倍率}.拡張子`。
- (NSString*)outputNameForPath:(NSString*)path apSize:(int)apSize {
    const double scales[] = {1.0, 1.5, 2.0, 3.0};
    const double scale = scales[[_drizzleSegment selectedSegment]];
    NSString* base = [[path lastPathComponent] stringByDeletingPathExtension];
    NSString* zoom = (scale == 1.0)
                         ? @""
                         : [NSString stringWithFormat:@"_x%@",
                                                      (scale == 1.5) ? @"1.5"
                                                                     : [NSString stringWithFormat:
                                                                                     @"%.0f", scale]];
    NSString* extension = [self currentOutputFormat] == OutputFormat::Png16 ? @"png" : @"tif";
    NSString* selection = _selectionUsesCount
                              ? [NSString stringWithFormat:@"%dframes", _apTopCountSetting]
                              : [NSString stringWithFormat:@"%.0fpct", _apTopPercentSetting];
    return [NSString stringWithFormat:@"%@_ap%d_%@%@.%@", base, apSize, selection, zoom,
                                      extension];
}

- (NSString*)outputDirectoryForPath:(NSString*)path {
    if (_outputDirectory) return _outputDirectory;
    return [path stringByDeletingLastPathComponent];
}

- (void)formatChanged:(id)sender {
    (void)sender;
    [self updateNamePreview];
}

- (void)updateNamePreview {
    if (_inputPath.empty()) {
        [_namePreview setStringValue:@""];
        return;
    }
    const int apSizes[] = {0, 32, 48, 64, 96, 128, 200};
    int ap = _analysis ? _analysis->ap_size : apSizes[[_apSizePopup indexOfSelectedItem]];
    if (ap == 0) ap = 64;
    [_namePreview setStringValue:[self outputNameForPath:[self inputPathString]
                                                  apSize:ap]];
}

- (void)chooseOutputDirectory:(id)sender {
    (void)sender;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setCanChooseDirectories:YES];
    [panel setCanChooseFiles:NO];
    if ([panel runModal] != NSModalResponseOK) return;
    [self setBatchOutputDirectory:[[[panel URLs] firstObject] path]];
}

- (void)resetOutputDirectory:(id)sender {
    (void)sender;
    [self setBatchOutputDirectory:nil];
    [_outputDirLabel setStringValue:LSLocalizedString(@"入力と同じフォルダ")];
}

- (void)save:(id)sender {
    (void)sender;
    if (!_displayed) return;

    NSSavePanel* panel = [NSSavePanel savePanel];
    const OutputFormat format = [self currentOutputFormat];
    [panel setAllowedFileTypes:format == OutputFormat::Png16 ? @[ @"png" ] : @[ @"tif" ]];
    [panel setNameFieldStringValue:[_namePreview stringValue]];
    if (_outputDirectory) [panel setDirectoryURL:[NSURL fileURLWithPath:_outputDirectory]];
    if ([panel runModal] != NSModalResponseOK) return;
    NSURL* url = [panel URL];
    if (!url) return;

    try {
        write_output_image(std::string([[url path] UTF8String]), *_displayed, format);
        [_statusLabel setStringValue:[NSString stringWithFormat:LSLocalizedString(@"保存しました: %@"),
                                                                [[url path] lastPathComponent]]];
    } catch (const std::exception& e) {
        [self showError:[NSString stringWithUTF8String:e.what()]
                  title:LSLocalizedString(@"保存できませんでした")];
    }
}

// ---- バッチ ---------------------------------------------------------------

- (void)batch:(id)sender {
    (void)sender;
    if (_running || [_items count] == 0) return;

    _running = YES;
    _batchRunning = YES;
    _stopQueue->store(false);
    _cancelFlag->store(false);
    [self resetEta];
    [_progress setDoubleValue:0.0];
    [_progress setHidden:NO];
    [self updateControlsEnabled];

    // 実行中に設定を触られても、キュー全体で同じ設定になるようにここで固める。
    const stackcore::OpenOptions openOptions = [self currentOpenOptions];
    stackcore::MapStackSettings settings = [self currentSettings];
    settings.ap.use_manual_points = false;  // ファイルごとに自動配置する
    settings.ap.manual_points.clear();
    const bool globalOnly = [_methodPopup indexOfSelectedItem] == 1;
    const bool lowMemory = [_lowMemoryCheck state] == NSControlStateValueOn;
    const OutputFormat format = [self currentOutputFormat];

    // 未処理・失敗のものだけを対象にする。
    // 途中で止めて掛け直したとき、済んだファイルをもう一度回さないため（仕様書 §5.1）。
    NSMutableArray* targets = [NSMutableArray array];
    for (NSUInteger i = 0; i < [_items count]; ++i) {
        QueueItem* item = _items[i];
        if ([item state] == QueueItemStateStacked) continue;
        [targets addObject:@(static_cast<long long>(i))];
    }
    if ([targets count] == 0) {
        _running = NO;
        _batchRunning = NO;
        [_statusLabel setStringValue:LSLocalizedString(@"すべて処理済みです")];
        [self updateControlsEnabled];
        if (_onRunFinished) _onRunFinished();
        return;
    }

    std::atomic<bool>* cancelFlag = _cancelFlag;
    std::atomic<bool>* stopQueue = _stopQueue;
    MainWindowController* controller = self;
    NSArray* frozenTargets = [targets copy];

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        const NSUInteger count = [frozenTargets count];
        for (NSUInteger n = 0; n < count; ++n) {
            if (stopQueue->load()) break;
            const NSUInteger index =
                static_cast<NSUInteger>([frozenTargets[n] longLongValue]);

            __block NSString* path = nil;
            dispatch_sync(dispatch_get_main_queue(), ^{
                path = [[controller queuePathAtIndex:index] copy];
                [controller beginBatchItem:index position:(n + 1) of:count];
            });
            if (!path) continue;

            cancelFlag->store(false);
            JobRequest req;
            req.path = std::string([path UTF8String]);
            req.options = openOptions;
            req.settings = settings;
            req.global_only = globalOnly;
            req.low_memory = lowMemory;
            req.want_stack = true;

            const stackcore::ProgressFn progress =
                [controller, cancelFlag](const char* stage, int done, int total) -> bool {
                if (cancelFlag->load()) return false;
                const int step = total < 100 ? 1 : total / 100;
                if (done % step == 0 || done == total) {
                    NSString* text = [NSString stringWithUTF8String:stage];
                    dispatch_async(dispatch_get_main_queue(), ^{
                        [controller reportStage:text done:done total:total];
                    });
                }
                return true;
            };

            JobResult result = run_job(req, progress);

            // 書き出しはバックグラウンドのまま行う。
            // 4K・Drizzle3倍だと数百MBになるので、メインキューでやると画面が止まる。
            std::string writeError;
            NSString* outPath = nil;
            if (!result.cancelled && result.error.empty() && result.image) {
                __block NSString* name = nil;
                __block NSString* dir = nil;
                const int apSize = result.analysis ? result.analysis->ap_size : 0;
                dispatch_sync(dispatch_get_main_queue(), ^{
                    name = [[controller outputNameForPath:path apSize:apSize] copy];
                    dir = [[controller outputDirectoryForPath:path] copy];
                });
                outPath = [[dir stringByAppendingPathComponent:name] retain];
                [name release];
                [dir release];
                try {
                    write_output_image(std::string([outPath UTF8String]), *result.image, format);
                } catch (const std::exception& e) {
                    writeError = e.what();
                }
            }

            NSString* finalPath = outPath;
            const bool cancelled = result.cancelled;
            const std::string error = !result.error.empty() ? result.error : writeError;
            dispatch_sync(dispatch_get_main_queue(), ^{
                [controller finishBatchItem:index
                                    outPath:finalPath
                                      error:[NSString stringWithUTF8String:error.c_str()]
                                  cancelled:cancelled];
            });
            [outPath release];
            [path release];
        }

        dispatch_async(dispatch_get_main_queue(), ^{
            [controller finishBatch];
        });
        [frozenTargets release];
    });
}

- (NSString*)queuePathAtIndex:(NSUInteger)index {
    if (index >= [_items count]) return nil;
    return [_items[index] path];
}

- (void)beginBatchItem:(NSUInteger)index position:(NSUInteger)position of:(NSUInteger)count {
    [self resetEta];
    [_queueTable selectRowIndexes:[NSIndexSet indexSetWithIndex:index] byExtendingSelection:NO];
    _currentIndex = static_cast<NSInteger>(index);
    [_statusLabel setStringValue:[NSString
                                     stringWithFormat:
                                         LSLocalizedString(@"[%lu/%lu] %@ を処理しています…"),
                                                            (unsigned long)position,
                                                            (unsigned long)count,
                                                            [[_items[index] path]
                                                                lastPathComponent]]];
}

- (void)finishBatchItem:(NSUInteger)index
                outPath:(NSString*)outPath
                  error:(NSString*)error
              cancelled:(bool)cancelled {
    if (index >= [_items count]) return;
    QueueItem* item = _items[index];
    if (cancelled) {
        [item setState:QueueItemStatePending];
        [item setMessage:LSLocalizedString(@"中断しました")];
    } else if ([error length] > 0) {
        [item setState:QueueItemStateError];
        [item setMessage:error];
    } else {
        [item setState:QueueItemStateStacked];
        [item setMessage:[NSString stringWithFormat:LSLocalizedString(@"→ %@"),
                                                    [outPath lastPathComponent]]];
    }
    [_queueTable reloadData];
}

- (void)finishBatch {
    _running = NO;
    _batchRunning = NO;
    [_progress setDoubleValue:0.0];
    [_progress setHidden:YES];
    [self resetEta];

    int done = 0, failed = 0, pending = 0;
    for (QueueItem* item in _items) {
        if ([item state] == QueueItemStateStacked) ++done;
        else if ([item state] == QueueItemStateError) ++failed;
        else ++pending;
    }
    NSString* text =
        [NSString stringWithFormat:
                      LSLocalizedString(@"バッチ終了 — 完了 %d / 失敗 %d / 残り %d"),
                      done, failed, pending];
    [_statusLabel setStringValue:text];
    [self notifyDone:text];
    [self updateControlsEnabled];
    if (_onRunFinished) _onRunFinished();
}

// ---- その他 ---------------------------------------------------------------

- (void)showError:(NSString*)message title:(NSString*)title {
    NSAlert* alert = [[[NSAlert alloc] init] autorelease];
    [alert setMessageText:LSLocalizedString(title)];
    [alert setInformativeText:message];
    [alert addButtonWithTitle:LSLocalizedString(@"OK")];
    [alert runModal];
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    if (_running) {
        _stopQueue->store(true);
        _cancelFlag->store(true);
        return NO;  // 中断が終わるまで閉じない
    }
    return YES;
}

@end
