#import "MainWindowController_Private.h"

#include <algorithm>
#include <cmath>

#include "stackcore/map_pipeline.hpp"
#include "stackcore/video_source.hpp"
#include "stackcore/wavelet.hpp"

@implementation MainWindowController (Layout)

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
    NSMenu* queueMenu = [[[NSMenu alloc] initWithTitle:@""] autorelease];
    [queueMenu setDelegate:self];
    for (NSMenuItem* item in @[
             [[[NSMenuItem alloc]
                 initWithTitle:LSLocalizedString(@"Finderで表示")
                        action:@selector(revealQueueItemInFinder:)
                 keyEquivalent:@""] autorelease],
             [[[NSMenuItem alloc]
                 initWithTitle:LSLocalizedString(@"キューから削除")
                        action:@selector(removeSelectedFromQueue:)
                 keyEquivalent:@""] autorelease]
         ]) {
        [item setTarget:self];
        [queueMenu addItem:item];
    }
    [_queueTable setMenu:queueMenu];
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
    _clearButton = [self buttonWithTitle:@"クリア" action:@selector(clearWorkspace:)];
    [_clearButton
        setToolTip:LSLocalizedString(
                       @"入力キューと処理結果を消去します。元動画・解析キャッシュ・書き出し済みファイル・設定は残ります")];
    [buttons addArrangedSubview:_clearButton];
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

    _bannerBar = [[[NSStackView alloc] init] autorelease];
    [_bannerBar setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
    [_bannerBar setSpacing:4.0];
    [_bannerBar setTranslatesAutoresizingMaskIntoConstraints:NO];

    _bannerLabel = MakeLabel(@"");
    [_bannerLabel setTextColor:[NSColor systemOrangeColor]];
    [_bannerLabel setLineBreakMode:NSLineBreakByTruncatingTail];
    [_bannerLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                            forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_bannerBar addArrangedSubview:_bannerLabel];

    _bannerLittleButton = [self buttonWithTitle:@"little" action:@selector(useLittleEndianFromBanner:)];
    _bannerBigButton = [self buttonWithTitle:@"big" action:@selector(useBigEndianFromBanner:)];
    _bannerDepthButton = [self buttonWithTitle:@"12bit" action:@selector(use12BitFromBanner:)];
    _bannerCloseButton = [self buttonWithTitle:@"×" action:@selector(dismissBanner:)];
    for (NSButton* button in
         @[ _bannerLittleButton, _bannerBigButton, _bannerDepthButton, _bannerCloseButton ]) {
        [button setControlSize:NSControlSizeSmall];
        [button setFont:[NSFont systemFontOfSize:10.0]];
        [button setHidden:YES];
        [_bannerBar addArrangedSubview:button];
    }
    [_bannerLittleButton setToolTip:LSLocalizedString(@"little endianとして読み直す")];
    [_bannerBigButton setToolTip:LSLocalizedString(@"big endianとして読み直す")];
    [_bannerDepthButton setToolTip:LSLocalizedString(@"12bitとして読み直す")];
    [_bannerCloseButton setToolTip:LSLocalizedString(@"警告を閉じる")];
    [pane addSubview:_bannerBar];

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
    [[_frameSlider widthAnchor] constraintGreaterThanOrEqualToConstant:140.0].active = YES;
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

    _apShowCheck = [self checkboxWithTitle:@"位置合わせ領域を表示" state:YES];
    [_apShowCheck setTarget:self];
    [_apShowCheck setAction:@selector(apDisplayChanged:)];
    [tools2 addArrangedSubview:_apShowCheck];

    _apHeatCheck = [self checkboxWithTitle:@"品質で色分け" state:NO];
    [_apHeatCheck setTarget:self];
    [_apHeatCheck setAction:@selector(apDisplayChanged:)];
    [tools2 addArrangedSubview:_apHeatCheck];

    _apEditCheck = [self checkboxWithTitle:@"配置を編集" state:NO];
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

    NSDictionary* views = NSDictionaryOfVariableBindings(_bannerBar, _preview, tools, tools2);
    for (NSString* format in
         @[ @"H:|[_bannerBar]|", @"H:|[_preview]|", @"H:|[tools]-(>=0)-|",
            @"H:|[tools2]-(>=0)-|" ]) {
        [pane addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:format
                                                                    options:0
                                                                    metrics:nil
                                                                      views:views]];
    }
    [pane addConstraints:[NSLayoutConstraint
                             constraintsWithVisualFormat:
                                 @"V:|[_bannerBar(22)]-4-[_preview(>=240)]-6-[tools]-4-[tools2]|"
                                                 options:0
                                                 metrics:nil
                                                   views:views]];
    return pane;
}

// --- 右: Inspector ---

- (NSView*)buildRightPane {
    NSView* pane = [[[NSView alloc] initWithFrame:NSZeroRect] autorelease];

    // 工程タブ。品質評価・位置合わせ・合成を別々に停止できるようにする。
    // 完了後は次のタブへ案内するが、次工程を勝手に実行はしない。
    _inspectorTab = [[[NSSegmentedControl alloc] init] autorelease];
    [_inspectorTab setSegmentCount:4];
    [_inspectorTab setLabel:@"品質評価" forSegment:0];
    [_inspectorTab setLabel:@"アライメント" forSegment:1];
    [_inspectorTab setLabel:@"スタック" forSegment:2];
    [_inspectorTab setLabel:@"仕上げ・出力" forSegment:3];
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

    [self buildQualitySection:box];
    [self buildAlignmentSection:box];
    [self buildStackSection:box];
    [self buildDrizzleSection:box];
    [self buildWaveletSection:box];
    [self buildExportSection:box];

    [self updateInspectorVisibility];
    return pane;
}

// セクションがどの工程タブに属するか。
- (NSInteger)tabIndexForSectionKey:(NSString*)key {
    if ([key isEqualToString:@"quality"]) return 0;
    if ([key isEqualToString:@"align"]) return 1;
    if ([key isEqualToString:@"stack"] || [key isEqualToString:@"drizzle"]) return 2;
    return 3;
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
    if ([key isEqualToString:@"align"]) title = @"アライメント（位置合わせ）";
    else if ([key isEqualToString:@"quality"]) title = @"各フレームの品質評価";
    else if ([key isEqualToString:@"stack"]) title = @"スタック（画像の合成）";
    else if ([key isEqualToString:@"drizzle"]) title = @"ドリズル拡大";
    else if ([key isEqualToString:@"wavelet"]) title = @"ウェーブレット仕上げ";
    else if ([key isEqualToString:@"export"]) title = @"書き出し";
    [header setTitle:[NSString stringWithFormat:@"%@ %@", mark, LSLocalizedString(title)]];
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
    [self beginSection:@"アライメント（位置合わせ）" key:key inBox:box];

    [self addToSection:key view:MakeLabel(@"位置合わせ方法") box:box];
    _methodPopup = [[[NSPopUpButton alloc] init] autorelease];
    [_methodPopup addItemWithTitle:@"複数領域の局所位置合わせ（推奨）"];
    [_methodPopup addItemWithTitle:@"画像全体の位置合わせのみ（高速）"];
    [_methodPopup setTranslatesAutoresizingMaskIntoConstraints:NO];
    [_methodPopup setTarget:self];
    [_methodPopup setAction:@selector(analysisSettingChanged:)];
    [self addToSection:key view:_methodPopup box:box];

    NSTextField* explanation =
        MakeLabel(@"位置合わせ領域とは、画像を小領域に分け、大気の揺らぎによる"
                  @"局所的なずれを別々に補正する単位です。");
    [explanation setTextColor:[NSColor secondaryLabelColor]];
    [[explanation cell] setWraps:YES];
    [explanation setPreferredMaxLayoutWidth:270.0];
    [self addToSection:key view:explanation box:box];

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

    [self addToSection:key view:MakeLabel(@"位置合わせ領域の大きさ") box:box];
    _apSizePopup = [[[NSPopUpButton alloc] init] autorelease];
    [_apSizePopup addItemWithTitle:@"自動"];
    for (NSString* s in @[ @"32", @"48", @"64", @"96", @"128", @"200" ]) {
        [_apSizePopup addItemWithTitle:[s stringByAppendingString:@" px"]];
    }
    [_apSizePopup setTarget:self];
    [_apSizePopup setAction:@selector(analysisSettingChanged:)];
    [_apSizePopup setTranslatesAutoresizingMaskIntoConstraints:NO];
    [self addToSection:key view:_apSizePopup box:box];

    [self addToSection:key view:MakeLabel(@"位置ずれの探索範囲") box:box];
    _searchRadiusSlider = [self sliderMin:8.0 max:32.0 value:16.0
                                   action:@selector(searchRadiusChanged:)];
    _searchRadiusValue = MakeLabel(@"±16");
    [self addToSection:key
                  view:[self row:_searchRadiusSlider trailing:_searchRadiusValue]
                   box:box];

    [self addToSection:key view:MakeLabel(@"位置合わせ領域の配置") box:box];
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
    [self beginSection:@"各フレームの品質評価" key:key inBox:box];

    [self addToSection:key view:MakeLabel(@"品質指標") box:box];
    _qualityMetricPopup = [[[NSPopUpButton alloc] init] autorelease];
    [_qualityMetricPopup addItemWithTitle:@"勾配エネルギー"];
    [_qualityMetricPopup addItemWithTitle:@"周波数帯パワー比"];
    [_qualityMetricPopup setTarget:self];
    [_qualityMetricPopup setAction:@selector(analysisSettingChanged:)];
    [_qualityMetricPopup setTranslatesAutoresizingMaskIntoConstraints:NO];
    [self addToSection:key view:_qualityMetricPopup box:box];

    // 入力の読み方は、品質評価の前に確定する。
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

- (void)buildStackSection:(NSStackView*)box {
    NSString* key = @"stack";
    [self beginSection:@"スタック（画像の合成）" key:key inBox:box];

    [self addToSection:key view:MakeLabel(@"フレームの選択方式") box:box];
    _selectionModeSegment = [[[NSSegmentedControl alloc] initWithFrame:NSZeroRect] autorelease];
    [_selectionModeSegment setSegmentCount:2];
    [_selectionModeSegment setLabel:@"割合" forSegment:0];
    [_selectionModeSegment setLabel:@"枚数" forSegment:1];
    [_selectionModeSegment setSelectedSegment:0];
    [_selectionModeSegment setTarget:self];
    [_selectionModeSegment setAction:@selector(selectionModeChanged:)];
    [_selectionModeSegment setTranslatesAutoresizingMaskIntoConstraints:NO];
    [self addToSection:key view:_selectionModeSegment box:box];

    _apTopCaption = MakeLabel(@"位置合わせ領域ごとに採用するフレーム（%）");
    [self addToSection:key view:_apTopCaption box:box];
    _apTopSlider = [self sliderMin:1.0 max:100.0 value:10.0 action:@selector(apTopChanged:)];
    _apTopValue = MakeLabel(@"10 %");
    [self addToSection:key view:[self row:_apTopSlider trailing:_apTopValue] box:box];

    NSTextField* note = MakeLabel(@"これだけを変えた再スタックは解析をやり直しません");
    [note setTextColor:[NSColor secondaryLabelColor]];
    [[note cell] setWraps:YES];
    [self addToSection:key view:note box:box];

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

}

- (void)buildDrizzleSection:(NSStackView*)box {
    NSString* key = @"drizzle";
    [self beginSection:@"ドリズル拡大" key:key inBox:box];

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

    [self addToSection:key view:MakeLabel(@"投影する画素の幅（pixfrac）") box:box];
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
    [self beginSection:@"ウェーブレット仕上げ" key:key inBox:box];

    _waveletPreviewCheck =
        [self checkboxWithTitle:@"ウェーブレット効果をプレビュー" state:YES];
    [_waveletPreviewCheck setTarget:self];
    [_waveletPreviewCheck setAction:@selector(waveletPreviewChanged:)];
    [self addToSection:key view:_waveletPreviewCheck box:box];

    NSTextField* compareNote =
        MakeLabel(@"ON/OFFは比較表示のみです。書き出しには設定中の効果を適用します。");
    [compareNote setTextColor:[NSColor secondaryLabelColor]];
    [[compareNote cell] setWraps:YES];
    [compareNote setPreferredMaxLayoutWidth:270.0];
    [self addToSection:key view:compareNote box:box];

    NSTextField* head =
        MakeLabel(@"上：細部強調（1.00＝変化なし、8以上は強め） / 下：ノイズ低減");
    [head setTextColor:[NSColor secondaryLabelColor]];
    [[head cell] setWraps:YES];
    [head setPreferredMaxLayoutWidth:270.0];
    [self addToSection:key view:head box:box];

    for (int j = 0; j < kWaveletLayers; ++j) {
        NSString* title = [NSString stringWithFormat:
                                        LSLocalizedString(@"Layer %d（約%d px）"),
                                        j + 1, 1 << (j + 1)];
        [self addToSection:key view:MakeLabel(title) box:box];

        _sharpenSliders[j] = [self sliderMin:0.0 max:kWaveletGuiSharpenMaximum value:1.0
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
    [self beginSection:@"書き出し" key:key inBox:box];

    [self addToSection:key view:MakeLabel(@"形式") box:box];
    _formatPopup = [[[NSPopUpButton alloc] init] autorelease];
    [_formatPopup addItemWithTitle:@"16bit TIFF"];
    [_formatPopup addItemWithTitle:@"32bit float TIFF"];
    [_formatPopup addItemWithTitle:@"32bit float FITS（PixInsight向け）"];
    [_formatPopup addItemWithTitle:@"16bit PNG"];
    [_formatPopup setTranslatesAutoresizingMaskIntoConstraints:NO];
    [_formatPopup setTarget:self];
    [_formatPopup setAction:@selector(formatChanged:)];
    [self addToSection:key view:_formatPopup box:box];

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

    _qualityButton = [self buttonWithTitle:@"品質評価" action:@selector(analyze:)];
    _alignButton = [self buttonWithTitle:@"アライメント" action:@selector(align:)];
    _stackButton = [self buttonWithTitle:@"スタック" action:@selector(run:)];
    [_stackButton setKeyEquivalent:@"\r"];
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

    for (NSView* v in @[ _qualityButton, _alignButton, _stackButton, _cancelButton,
                         _progress, _statusLabel ]) {
        [bar addSubview:v];
    }

    NSDictionary* views = NSDictionaryOfVariableBindings(_qualityButton, _alignButton,
                                                         _stackButton,
                                                         _cancelButton, _progress, _statusLabel);
    [bar addConstraints:[NSLayoutConstraint
                            constraintsWithVisualFormat:
                                @"H:|[_qualityButton(>=76)]-6-[_alignButton(>=96)]-6-[_stackButton(>=76)"
                                @"]-12-[_progress(>=120)]-10-[_statusLabel(>=180)]-8-"
                                @"[_cancelButton(>=60)]|"
                                                options:NSLayoutFormatAlignAllCenterY
                                                metrics:nil
                                                  views:views]];
    [bar addConstraints:[NSLayoutConstraint
                            constraintsWithVisualFormat:@"V:|-8-[_qualityButton]-(>=0)-|"
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

@end
