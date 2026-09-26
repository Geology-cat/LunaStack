#import "MainWindowController_Private.h"

#include <algorithm>
#include <cmath>

// 既定で畳んでおくセクション（詳細設定）。普段は触らないつまみで画面を埋めない。
static BOOL LSSectionClosedByDefault(NSString* key) {
    return [key isEqualToString:@"qualityAdvanced"] || [key isEqualToString:@"alignAdvanced"] ||
           [key isEqualToString:@"input"];
}

@implementation MainWindowController (Layout)

// ---- 画面の組み立て -------------------------------------------------------

- (void)buildInterface {
    NSView* content = [[self window] contentView];

    // コンテンツビュー自身に背景を持たせる。
    // ウィンドウの背景は本来ウィンドウ枠側が描くので、
    // コンテンツビューだけを画像に落とすと背景が抜ける
    // （自己検証のスナップショットが真っ黒になって気づいた）。
    [content setWantsLayer:YES];
    [[content layer] setBackgroundColor:[[NSColor windowBackgroundColor] CGColor]];

    _leftPane = [self buildLeftPane];
    NSView* center = [self buildCenterPane];
    _rightPane = [self buildRightPane];
    NSView* status = [self buildStatusBar];

    for (NSView* v in @[ _leftPane, center, _rightPane, status ]) {
        [v setTranslatesAutoresizingMaskIntoConstraints:NO];
        [content addSubview:v];
    }

    // 3ペインは幅を固定＋中央可変にする。
    //
    // NSSplitView を使わないのは、10.13から最新まで同じ見た目で動くことを
    // 優先したため。分割線のドラッグより「どの環境でも壊れない」ことを取る。
    // 左右は⌘1/⌘2で畳める（UI設計書 §2 の「折りたたみ可能」）。
    // 畳むときは幅と間隔の制約を0にするので、参照を持っておく。
    _leftWidth = [[_leftPane widthAnchor] constraintEqualToConstant:220.0];
    _leftGap = [[center leadingAnchor] constraintEqualToAnchor:[_leftPane trailingAnchor]
                                                      constant:8.0];
    _rightWidth = [[_rightPane widthAnchor] constraintEqualToConstant:300.0];
    _rightGap = [[_rightPane leadingAnchor] constraintEqualToAnchor:[center trailingAnchor]
                                                           constant:8.0];
    NSArray* constraints = @[
        [[_leftPane leadingAnchor] constraintEqualToAnchor:[content leadingAnchor] constant:10.0],
        _leftWidth, _leftGap, _rightGap, _rightWidth,
        [[content trailingAnchor] constraintEqualToAnchor:[_rightPane trailingAnchor] constant:10.0],
        [[center widthAnchor] constraintGreaterThanOrEqualToConstant:360.0],
        [[_leftPane topAnchor] constraintEqualToAnchor:[content topAnchor] constant:10.0],
        [[center topAnchor] constraintEqualToAnchor:[content topAnchor] constant:10.0],
        [[_rightPane topAnchor] constraintEqualToAnchor:[content topAnchor] constant:10.0],
        [[status topAnchor] constraintEqualToAnchor:[_leftPane bottomAnchor] constant:8.0],
        [[status topAnchor] constraintEqualToAnchor:[center bottomAnchor] constant:8.0],
        [[status topAnchor] constraintEqualToAnchor:[_rightPane bottomAnchor] constant:8.0],
        [[status heightAnchor] constraintEqualToConstant:46.0],
        [[content bottomAnchor] constraintEqualToAnchor:[status bottomAnchor] constant:8.0],
        [[status leadingAnchor] constraintEqualToAnchor:[content leadingAnchor] constant:10.0],
        [[content trailingAnchor] constraintEqualToAnchor:[status trailingAnchor] constant:10.0],
    ];
    [NSLayoutConstraint activateConstraints:constraints];

    [self updateControlsEnabled];
    [self updateDrizzleEstimate];
    [self updateNamePreview];
    [self updateFinishingValueLabels];
    LSLocalizeViewTree(content);
    [self refreshCutLabel];
}

- (void)toggleLeftPane:(id)sender {
    (void)sender;
    const BOOL hide = ![_leftPane isHidden];
    [_leftPane setHidden:hide];
    [_leftWidth setConstant:hide ? 0.0 : 220.0];
    [_leftGap setConstant:hide ? 0.0 : 8.0];
}

- (void)toggleRightPane:(id)sender {
    (void)sender;
    const BOOL hide = ![_rightPane isHidden];
    [_rightPane setHidden:hide];
    [_rightWidth setConstant:hide ? 0.0 : 300.0];
    [_rightGap setConstant:hide ? 0.0 : 8.0];
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
                       @"入力キューと処理結果を消去し、すべての設定と仕上げを初期値に戻します。元動画・解析キャッシュ・書き出し済みファイル・プリセットは残ります")];
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

    _graphHint = MakeLabel(@"オレンジの線は参照画像に使う上位の割合です。線をドラッグで変更、ほかの場所をクリックでそのフレームを表示します");
    [_graphHint setTextColor:[NSColor secondaryLabelColor]];
    [_graphHint setLineBreakMode:NSLineBreakByWordWrapping];
    [[_graphHint cell] setWraps:YES];
    [_graphHint setPreferredMaxLayoutWidth:218.0];
    [pane addSubview:_graphHint];

    NSDictionary* views = NSDictionaryOfVariableBindings(queueTitle, scroll, buttons, graphTitle,
                                                         _graphMode, _graph, _graphHint);
    for (NSString* format in @[
             @"H:|[queueTitle]|", @"H:|[scroll]|", @"H:|[buttons]|", @"H:|[graphTitle]|",
             @"H:|[_graphMode]|", @"H:|[_graph]|", @"H:|[_graphHint]|"
         ]) {
        [pane addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:format
                                                                    options:0
                                                                    metrics:nil
                                                                      views:views]];
    }
    // キューは必要な分だけ、残りはグラフに回す（グラフは細部の読み取りに高さが要る）。
    [pane addConstraints:
              [NSLayoutConstraint
                  constraintsWithVisualFormat:@"V:|[queueTitle]-4-[scroll(>=96)]-4-[buttons]-12-"
                                              @"[graphTitle]-4-[_graphMode]-4-[_graph(>=140)]-4-"
                                              @"[_graphHint]|"
                                      options:0
                                      metrics:nil
                                        views:views]];
    NSLayoutConstraint* queueHeight = [[scroll heightAnchor] constraintEqualToConstant:170.0];
    [queueHeight setPriority:NSLayoutPriorityDefaultHigh - 1];
    [queueHeight setActive:YES];
    return pane;
}

// --- 中央: プレビュー ---

- (NSView*)buildCenterPane {
    NSView* pane = [[[NSView alloc] initWithFrame:NSZeroRect] autorelease];

    _bannerBar = [[[NSStackView alloc] init] autorelease];
    [_bannerBar setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
    [_bannerBar setSpacing:4.0];
    [_bannerBar setTranslatesAutoresizingMaskIntoConstraints:NO];
    // 幅が足りなければ文を切り詰める。バナーがウィンドウの最小幅を押し広げてはいけない。
    [_bannerBar setClippingResistancePriority:NSLayoutPriorityDefaultLow
                               forOrientation:NSLayoutConstraintOrientationHorizontal];

    _bannerLabel = MakeLabel(@"");
    [_bannerLabel setTextColor:[NSColor systemOrangeColor]];
    [_bannerLabel setLineBreakMode:NSLineBreakByTruncatingTail];
    [_bannerLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow - 10
                                            forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_bannerBar addArrangedSubview:_bannerLabel];

    _bannerLittleButton = [self buttonWithTitle:@"little" action:@selector(useLittleEndianFromBanner:)];
    _bannerBigButton = [self buttonWithTitle:@"big" action:@selector(useBigEndianFromBanner:)];
    _bannerDepthButton = [self buttonWithTitle:@"12bit" action:@selector(use12BitFromBanner:)];
    _bannerDetailsButton = [self buttonWithTitle:@"詳細…" action:@selector(showRejectedFrames:)];
    _bannerCloseButton = [self buttonWithTitle:@"×" action:@selector(dismissBanner:)];
    for (NSButton* button in @[ _bannerLittleButton, _bannerBigButton, _bannerDepthButton,
                                _bannerDetailsButton, _bannerCloseButton ]) {
        [button setControlSize:NSControlSizeSmall];
        [button setFont:[NSFont systemFontOfSize:10.0]];
        [button setHidden:YES];
        [_bannerBar addArrangedSubview:button];
    }
    [_bannerLittleButton setToolTip:LSLocalizedString(@"little endianとして読み直す")];
    [_bannerBigButton setToolTip:LSLocalizedString(@"big endianとして読み直す")];
    [_bannerDepthButton setToolTip:LSLocalizedString(@"12bitとして読み直す")];
    [_bannerDetailsButton setToolTip:LSLocalizedString(@"除外したフレームと理由を一覧で見る")];
    [_bannerCloseButton setToolTip:LSLocalizedString(@"警告を閉じる")];
    [pane addSubview:_bannerBar];

    _preview = [[[PreviewView alloc] initWithFrame:NSZeroRect] autorelease];
    [_preview setTranslatesAutoresizingMaskIntoConstraints:NO];
    [_preview setDelegate:self];
    [pane addSubview:_preview];

    // ツールは2段に分ける。1段に並べると、最小ウィンドウ幅
    // （UI設計書 §2 の 1000pt）に収まらず、ウィンドウが勝手に広がってしまう。
    // 説明ラベルは真っ先に縮むようにし、段そのものも幅が足りなければ切り詰める。
    NSStackView* tools = [[[NSStackView alloc] init] autorelease];
    NSStackView* tools2 = [[[NSStackView alloc] init] autorelease];
    for (NSStackView* row in @[ tools, tools2 ]) {
        [row setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
        [row setSpacing:8.0];
        [row setTranslatesAutoresizingMaskIntoConstraints:NO];
        [row setClippingResistancePriority:NSLayoutPriorityDefaultLow
                            forOrientation:NSLayoutConstraintOrientationHorizontal];
    }

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

    // スライダーの並び。品質評価の後は品質順で送ると、良いフレームと悪いフレームを
    // 見比べやすい（左端が最良、右へ行くほど悪い）。
    _frameOrderSegment = [[[NSSegmentedControl alloc] init] autorelease];
    [_frameOrderSegment setSegmentCount:2];
    [_frameOrderSegment setLabel:@"時系列" forSegment:0];
    [_frameOrderSegment setLabel:@"品質順" forSegment:1];
    [_frameOrderSegment setSelectedSegment:0];
    [_frameOrderSegment setControlSize:NSControlSizeSmall];
    [_frameOrderSegment setFont:[NSFont systemFontOfSize:10.0]];
    [_frameOrderSegment setTarget:self];
    [_frameOrderSegment setAction:@selector(frameOrderChanged:)];
    [_frameOrderSegment setTranslatesAutoresizingMaskIntoConstraints:NO];
    [_frameOrderSegment setToolTip:LSLocalizedString(@"スライダーでフレームを送る順番（品質評価の後は品質順にできます）")];
    [tools addArrangedSubview:_frameOrderSegment];

    _frameSlider = [self sliderMin:0.0 max:0.0 value:0.0 action:@selector(frameSliderChanged:)];
    [[_frameSlider widthAnchor] constraintGreaterThanOrEqualToConstant:60.0].active = YES;
    [_frameSlider setContentHuggingPriority:NSLayoutPriorityDefaultLow - 1
                             forOrientation:NSLayoutConstraintOrientationHorizontal];
    [tools addArrangedSubview:_frameSlider];

    _frameInfoLabel = MakeLabel(@"");
    [_frameInfoLabel setLineBreakMode:NSLineBreakByTruncatingTail];
    [_frameInfoLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow - 5
                                               forOrientation:NSLayoutConstraintOrientationHorizontal];
    [[_frameInfoLabel widthAnchor] constraintLessThanOrEqualToConstant:260.0].active = YES;
    // 「上位 x.x%」の部分だけは必ず見えるようにする（狭いときはスライダーを縮める）。
    NSLayoutConstraint* infoMin = [[_frameInfoLabel widthAnchor] constraintGreaterThanOrEqualToConstant:96.0];
    [infoMin setPriority:NSLayoutPriorityDefaultHigh];
    [infoMin setActive:YES];
    [tools addArrangedSubview:_frameInfoLabel];

    _zoomControl = [[[NSSegmentedControl alloc] init] autorelease];
    [_zoomControl setSegmentCount:4];
    NSArray* zoomLabels = @[ @"全体", @"100%", @"200%", @"400%" ];
    for (NSUInteger i = 0; i < [zoomLabels count]; ++i) {
        [_zoomControl setLabel:zoomLabels[i] forSegment:static_cast<NSInteger>(i)];
    }
    [_zoomControl setSelectedSegment:0];
    [_zoomControl setTarget:self];
    [_zoomControl setAction:@selector(zoomChanged:)];
    [_zoomControl setTranslatesAutoresizingMaskIntoConstraints:NO];
    [_zoomControl setToolTip:LSLocalizedString(@"100%は画面の実画素で等倍。⌘＋ホイールやピンチでも拡大縮小できます")];
    [tools2 addArrangedSubview:_zoomControl];

    // 表示の切り替えはプルダウンにまとめる（チェックボックスを並べると最小幅に収まらない）。
    _displayMenu = [[[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES] autorelease];
    [_displayMenu setTranslatesAutoresizingMaskIntoConstraints:NO];
    [_displayMenu addItemWithTitle:@"表示"];
    for (NSString* title in @[ @"位置合わせ領域を表示", @"品質で色分け", @"表示を明るくする（保存には影響しません）" ]) {
        [_displayMenu addItemWithTitle:title];
        NSMenuItem* item = [_displayMenu lastItem];
        [item setTarget:self];
        [item setAction:@selector(displayMenuChanged:)];
    }
    [[_displayMenu itemAtIndex:1] setState:NSControlStateValueOn];
    [[_displayMenu itemAtIndex:3] setState:NSControlStateValueOn];
    [tools2 addArrangedSubview:_displayMenu];

    _apEditCheck = [self checkboxWithTitle:@"配置を編集" state:NO];
    [_apEditCheck setTarget:self];
    [_apEditCheck setAction:@selector(apDisplayChanged:)];
    [_apEditCheck setToolTip:LSLocalizedString(@"クリックで位置合わせ領域を追加、選んでDeleteで削除（⌘Zで取り消し）")];
    [tools2 addArrangedSubview:_apEditCheck];

    _apCountLabel = MakeLabel(@"");
    [_apCountLabel setTextColor:[NSColor secondaryLabelColor]];
    [_apCountLabel setLineBreakMode:NSLineBreakByTruncatingTail];
    [_apCountLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow - 20
                                            forOrientation:NSLayoutConstraintOrientationHorizontal];
    [tools2 addArrangedSubview:_apCountLabel];

    _pixelLabel = MakeLabel(@"");
    [_pixelLabel setTextColor:[NSColor secondaryLabelColor]];
    [_pixelLabel setFont:[NSFont monospacedDigitSystemFontOfSize:10.0 weight:NSFontWeightRegular]];
    [_pixelLabel setLineBreakMode:NSLineBreakByTruncatingTail];
    [_pixelLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow - 30
                                          forOrientation:NSLayoutConstraintOrientationHorizontal];
    [tools2 addArrangedSubview:_pixelLabel];

    [pane addSubview:tools];
    [pane addSubview:tools2];

    NSDictionary* views = NSDictionaryOfVariableBindings(_bannerBar, _preview, tools, tools2);
    for (NSString* format in
         @[ @"H:|[_bannerBar]|", @"H:|[_preview]|", @"H:|[tools]|", @"H:|[tools2]-(>=0)-|" ]) {
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

    // プリセットは全工程の設定をまとめて保存するので、工程タブの外に置く。
    NSStackView* presetRow = [[[NSStackView alloc] init] autorelease];
    [presetRow setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
    [presetRow setSpacing:6.0];
    [presetRow setTranslatesAutoresizingMaskIntoConstraints:NO];
    _presetPopup = [[[NSPopUpButton alloc] init] autorelease];
    [_presetPopup setTranslatesAutoresizingMaskIntoConstraints:NO];
    [_presetPopup setTarget:self];
    [_presetPopup setAction:@selector(presetSelected:)];
    [presetRow addArrangedSubview:_presetPopup];
    [presetRow addArrangedSubview:[self buttonWithTitle:@"プリセットを保存…"
                                                 action:@selector(savePreset:)]];
    [pane addSubview:presetRow];

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

    // 設定パネルは縦にだけスクロールする（トラックパッドの横スワイプで左右へずらさない）。
    NSScrollView* scroll = [[[NSScrollView alloc] initWithFrame:NSZeroRect] autorelease];
    VerticalClipView* clip = [[[VerticalClipView alloc] initWithFrame:NSZeroRect] autorelease];
    [clip setDrawsBackground:NO];
    [scroll setContentView:clip];
    [scroll setHasVerticalScroller:YES];
    [scroll setHasHorizontalScroller:NO];
    [scroll setHorizontalScrollElasticity:NSScrollElasticityNone];
    [scroll setDrawsBackground:NO];
    [scroll setTranslatesAutoresizingMaskIntoConstraints:NO];
    [pane addSubview:scroll];
    _inspectorScroll = scroll;

    NSDictionary* views = NSDictionaryOfVariableBindings(presetRow, _inspectorTab, scroll);
    for (NSString* format in @[ @"H:|[presetRow]-(>=0)-|", @"H:|[_inspectorTab]|", @"H:|[scroll]|",
                                @"V:|[presetRow]-6-[_inspectorTab]-6-[scroll]|" ]) {
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

    // 幅はスクロールビュー全体ではなく表示領域（クリップビュー）に合わせる。
    // スクロールバーを常に表示する設定では、全体の幅に合わせるとバーの幅だけ
    // 中身がはみ出し、横にずらせてしまう。
    [[[container widthAnchor] constraintEqualToAnchor:[clip widthAnchor]] setActive:YES];
    [[[box topAnchor] constraintEqualToAnchor:[container topAnchor] constant:2.0] setActive:YES];
    [[[box leadingAnchor] constraintEqualToAnchor:[container leadingAnchor]
                                         constant:2.0] setActive:YES];
    [[[container bottomAnchor] constraintEqualToAnchor:[box bottomAnchor]
                                              constant:4.0] setActive:YES];
    [[[box widthAnchor] constraintEqualToConstant:278.0] setActive:YES];

    [self buildQualitySection:box];
    [self buildInputSection:box];
    [self buildQualityAdvancedSection:box];
    [self buildAlignmentSection:box];
    [self buildAlignmentAdvancedSection:box];
    [self buildStackSection:box];
    [self buildDrizzleSection:box];
    [self buildCompareSection:box];
    [self buildChannelSection:box];
    [self buildWaveletSection:box];
    [self buildColorSection:box];
    [self buildToneSection:box];
    [self buildGeometrySection:box];
    [self buildExportSection:box];

    [self updateInspectorVisibility];
    return pane;
}

// セクションがどの工程タブに属するか。
- (NSInteger)tabIndexForSectionKey:(NSString*)key {
    if ([key isEqualToString:@"quality"] || [key isEqualToString:@"input"] ||
        [key isEqualToString:@"qualityAdvanced"]) {
        return 0;
    }
    if ([key isEqualToString:@"align"] || [key isEqualToString:@"alignAdvanced"]) return 1;
    if ([key isEqualToString:@"stack"] || [key isEqualToString:@"drizzle"]) return 2;
    return 3;
}

- (void)inspectorTabChanged:(id)sender {
    (void)sender;
    [self updateInspectorVisibility];
}

- (void)selectInspectorTab:(NSInteger)tab {
    [_inspectorTab setSelectedSegment:tab];
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
    [box setCustomSpacing:4.0 afterView:header];

    _sections[key] = [NSMutableArray array];
    _sectionHeaders[key] = header;
    [header setToolTip:title];
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
    if (!value) return !LSSectionClosedByDefault(key);
    return [value boolValue];
}

- (void)updateSectionHeader:(NSButton*)header key:(NSString*)key {
    NSString* mark = [self sectionOpen:key] ? @"▼" : @"▶";
    [header setTitle:[NSString stringWithFormat:@"%@ %@", mark,
                                                LSLocalizedString([header toolTip])]];
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

// 補足説明（灰色・折り返し）。
- (NSTextField*)noteLabel:(NSString*)text {
    NSTextField* note = MakeLabel(text);
    [note setTextColor:[NSColor secondaryLabelColor]];
    [[note cell] setWraps:YES];
    [note setPreferredMaxLayoutWidth:270.0];
    return note;
}

// 「見出し 数値欄」の1行。
- (NSView*)labeledField:(NSString*)caption field:(NSTextField*)field {
    NSTextField* label = MakeLabel(caption);
    [[label widthAnchor] constraintEqualToConstant:170.0].active = YES;
    return [self row:label trailing:field];
}

// 「見出し スライダー 値」の1行。見出しを付けて、どのつまみかを明示する。
- (NSView*)captionRow:(NSString*)caption slider:(NSSlider*)slider value:(NSTextField*)value {
    NSStackView* row = [[[NSStackView alloc] init] autorelease];
    [row setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
    [row setSpacing:6.0];
    [row setTranslatesAutoresizingMaskIntoConstraints:NO];
    NSTextField* label = MakeLabel(caption);
    [label setTextColor:[NSColor secondaryLabelColor]];
    [[label widthAnchor] constraintEqualToConstant:44.0].active = YES;
    [row addArrangedSubview:label];
    [row addArrangedSubview:slider];
    [row addArrangedSubview:value];
    [[value widthAnchor] constraintEqualToConstant:40.0].active = YES;
    [[row widthAnchor] constraintEqualToConstant:272.0].active = YES;
    return row;
}

// 数値欄（直接入力できる）。Return か入力欄を離れたときに反映する。
- (NSTextField*)valueFieldWithIndex:(int)index {
    NSTextField* field = [[[NSTextField alloc] init] autorelease];
    [field setFont:[NSFont monospacedDigitSystemFontOfSize:11.0 weight:NSFontWeightRegular]];
    [field setAlignment:NSTextAlignmentRight];
    [field setControlSize:NSControlSizeSmall];
    [field setTranslatesAutoresizingMaskIntoConstraints:NO];
    [field setTag:index];
    [field setTarget:self];
    [field setAction:@selector(adjustFieldChanged:)];
    [[field cell] setSendsActionOnEndEditing:YES];
    return field;
}

// 「見出し －［スライダー］＋ 数値欄」の1行。
// －/＋は押し続けると連続して動く（NSButton の continuous。押している時間が長いほど速く）。
- (NSView*)adjustRow:(NSString*)caption slider:(NSSlider*)slider field:(NSTextField*)field
               index:(int)index step:(double)step {
    NSStackView* row = [[[NSStackView alloc] init] autorelease];
    [row setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
    [row setSpacing:3.0];
    [row setTranslatesAutoresizingMaskIntoConstraints:NO];
    NSTextField* label = MakeLabel(caption);
    [label setTextColor:[NSColor secondaryLabelColor]];
    [[label cell] setWraps:NO];
    [label setLineBreakMode:NSLineBreakByClipping];
    [[label widthAnchor] constraintEqualToConstant:40.0].active = YES;
    NSButton* minus = [self buttonWithTitle:@"－" action:@selector(adjustMinus:)];
    NSButton* plus = [self buttonWithTitle:@"＋" action:@selector(adjustPlus:)];
    for (NSButton* b in @[ minus, plus ]) {
        [b setBezelStyle:NSBezelStyleSmallSquare];
        [b setControlSize:NSControlSizeSmall];
        [b setTag:index];
        [b setContinuous:YES];
        [b setPeriodicDelay:0.35f interval:0.06f];
        [[b widthAnchor] constraintEqualToConstant:20.0].active = YES;
    }
    [minus setToolTip:LSLocalizedString(@"少し下げる（押し続けると連続）")];
    [plus setToolTip:LSLocalizedString(@"少し上げる（押し続けると連続）")];
    [row addArrangedSubview:label];
    [row addArrangedSubview:minus];
    [row addArrangedSubview:slider];
    [row addArrangedSubview:plus];
    [row addArrangedSubview:field];
    [[field widthAnchor] constraintEqualToConstant:46.0].active = YES;
    [[row widthAnchor] constraintEqualToConstant:272.0].active = YES;
    _adjSliders[index] = slider;
    _adjFields[index] = field;
    _adjMinus[index] = minus;
    _adjPlus[index] = plus;
    _adjSteps[index] = step;
    return row;
}

- (NSStackView*)buttonRow:(NSArray*)buttons {
    NSStackView* row = [[[NSStackView alloc] init] autorelease];
    [row setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
    [row setSpacing:6.0];
    [row setTranslatesAutoresizingMaskIntoConstraints:NO];
    for (NSView* b in buttons) [row addArrangedSubview:b];
    return row;
}

// ---- 品質評価タブ ----------------------------------------------------------

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

- (void)buildInputSection:(NSStackView*)box {
    NSString* key = @"input";
    [self beginSection:@"入力の前処理（範囲・色・補正）" key:key inBox:box];

    [self addToSection:key view:MakeLabel(@"使うフレームの範囲（空欄で全体）") box:box];
    _rangeStartField = [self numberFieldWithValue:@"" action:@selector(inputInterpretationChanged:)];
    _rangeEndField = [self numberFieldWithValue:@"" action:@selector(inputInterpretationChanged:)];
    [_rangeStartField setPlaceholderString:@"1"];
    [_rangeEndField setPlaceholderString:LSLocalizedString(@"最後")];
    NSTextField* dash = MakeLabel(@"〜");
    [self addToSection:key view:[self buttonRow:@[ _rangeStartField, dash, _rangeEndField ]] box:box];

    [self addToSection:key view:MakeLabel(@"色の配列（Bayer）") box:box];
    _bayerPopup = [[[NSPopUpButton alloc] init] autorelease];
    for (NSString* t in @[ @"ヘッダに従う", @"モノクロとして扱う", @"RGGB", @"GRBG", @"GBRG", @"BGGR" ]) {
        [_bayerPopup addItemWithTitle:t];
    }
    [_bayerPopup setTranslatesAutoresizingMaskIntoConstraints:NO];
    [_bayerPopup setTarget:self];
    [_bayerPopup setAction:@selector(inputInterpretationChanged:)];
    [self addToSection:key view:_bayerPopup box:box];

    [self addToSection:key view:MakeLabel(@"デバイヤーの方式") box:box];
    _debayerPopup = [[[NSPopUpButton alloc] init] autorelease];
    [_debayerPopup addItemWithTitle:@"標準（bilinear）"];
    [_debayerPopup addItemWithTitle:@"高品質（Malvar-He-Cutler）"];
    [_debayerPopup setTranslatesAutoresizingMaskIntoConstraints:NO];
    [_debayerPopup setTarget:self];
    [_debayerPopup setAction:@selector(inputInterpretationChanged:)];
    [self addToSection:key view:_debayerPopup box:box];

    [self addToSection:key view:MakeLabel(@"ダーク（熱かぶり・ホットピクセルの除去）") box:box];
    _darkLabel = MakeLabel(@"なし");
    [_darkLabel setLineBreakMode:NSLineBreakByTruncatingMiddle];
    [_darkLabel setTextColor:[NSColor secondaryLabelColor]];
    [self addToSection:key view:_darkLabel box:box];
    [self addToSection:key view:[self buttonRow:@[ [self buttonWithTitle:@"ダークを選ぶ…" action:@selector(chooseDark:)] ]] box:box];

    [self addToSection:key view:MakeLabel(@"フラット（周辺減光・ホコリの影の補正）") box:box];
    _flatLabel = MakeLabel(@"なし");
    [_flatLabel setLineBreakMode:NSLineBreakByTruncatingMiddle];
    [_flatLabel setTextColor:[NSColor secondaryLabelColor]];
    [self addToSection:key view:_flatLabel box:box];
    [self addToSection:key
                  view:[self buttonRow:@[ [self buttonWithTitle:@"フラットを選ぶ…" action:@selector(chooseFlat:)],
                                          [self buttonWithTitle:@"補正をやめる" action:@selector(clearCalibration:)] ]]
                   box:box];
    [self addToSection:key
                  view:[self noteLabel:@"ダーク・フラットには動画・静止画・静止画のフォルダを使えます。全フレームを平均してマスターを作り、デバイヤー前の画素に掛けます。"]
                   box:box];
}

- (void)buildQualityAdvancedSection:(NSStackView*)box {
    NSString* key = @"qualityAdvanced";
    [self beginSection:@"詳細設定（追跡失敗の判定）" key:key inBox:box];
    _outlierKField = [self numberFieldWithValue:@"6.0" action:@selector(advancedFieldChanged:)];
    [self addToSection:key view:[self labeledField:@"外れ値判定の厳しさ（σ）" field:_outlierKField] box:box];
    _minSimilarityField = [self numberFieldWithValue:@"0.5" action:@selector(advancedFieldChanged:)];
    [self addToSection:key view:[self labeledField:@"参照との類似度の下限" field:_minSimilarityField] box:box];
    _maxShiftField = [self numberFieldWithValue:@"" action:@selector(advancedFieldChanged:)];
    [_maxShiftField setPlaceholderString:LSLocalizedString(@"自動")];
    [self addToSection:key view:[self labeledField:@"許容する位置ずれ（px）" field:_maxShiftField] box:box];
    [self addToSection:key
                  view:[self noteLabel:@"外れ値判定を小さくすると、崩れたフレームをより厳しく除外します。位置ずれの上限は既定で短辺の1/4です。"]
                   box:box];
}

// ---- アライメントタブ ------------------------------------------------------

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

    [self addToSection:key
                  view:[self noteLabel:@"位置合わせ領域とは、画像を小領域に分け、大気の揺らぎによる局所的なずれを別々に補正する単位です。"]
                   box:box];

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
    for (int i = 1; i < kApSizeChoiceCount; ++i) {
        [_apSizePopup addItemWithTitle:[NSString stringWithFormat:@"%d px", LSApSizeAt(i)]];
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
    [self addToSection:key
                  view:[self buttonRow:@[ [self buttonWithTitle:@"自動配置に戻す" action:@selector(resetApPlacement:)],
                                          [self buttonWithTitle:@"すべて消去" action:@selector(clearApPlacement:)] ]]
                   box:box];

    [self addToSection:key view:MakeLabel(@"参照フレーム（品質上位 %）") box:box];
    _topSlider = [self sliderMin:5.0 max:50.0 value:25.0 action:@selector(topChanged:)];
    _topValue = MakeLabel(@"25 %");
    [self addToSection:key view:[self row:_topSlider trailing:_topValue] box:box];

    _refineCheck = [self checkboxWithTitle:@"参照の反復精密化（2パス）" state:YES];
    [_refineCheck setTarget:self];
    [_refineCheck setAction:@selector(analysisSettingChanged:)];
    [self addToSection:key view:_refineCheck box:box];

    // アライメントの内訳（U7）。自動判定したモードや補間の割合を結果として見せる。
    _alignSummaryLabel = [self noteLabel:@""];
    [_alignSummaryLabel setSelectable:YES];
    [self addToSection:key view:_alignSummaryLabel box:box];
}

- (void)buildAlignmentAdvancedSection:(NSStackView*)box {
    NSString* key = @"alignAdvanced";
    [self beginSection:@"詳細設定（位置合わせ領域）" key:key inBox:box];
    _minScoreField = [self numberFieldWithValue:@"0.5" action:@selector(advancedFieldChanged:)];
    [self addToSection:key view:[self labeledField:@"一致度の下限（下回ると補間）" field:_minScoreField] box:box];
    _apGradientField = [self numberFieldWithValue:@"0.6" action:@selector(advancedFieldChanged:)];
    [self addToSection:key view:[self labeledField:@"配置する模様の強さ（比）" field:_apGradientField] box:box];
    _apLevelField = [self numberFieldWithValue:@"0.15" action:@selector(advancedFieldChanged:)];
    [self addToSection:key view:[self labeledField:@"配置する明るさ（比）" field:_apLevelField] box:box];
    [self addToSection:key
                  view:[self noteLabel:@"模様の強さ・明るさを下げると、暗い所や模様の乏しい所にも位置合わせ領域を置きます。"]
                   box:box];
}

// ---- スタックタブ ----------------------------------------------------------

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

    [self addToSection:key view:[self noteLabel:@"これだけを変えた再スタックは解析をやり直しません"] box:box];

    _normalizeCheck = [self checkboxWithTitle:@"輝度正規化" state:YES];
    [_normalizeCheck setTarget:self];
    [_normalizeCheck setAction:@selector(analysisSettingChanged:)];
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

    _sigmaField = [self numberFieldWithValue:@"2.0" action:@selector(advancedFieldChanged:)];
    [self addToSection:key view:[self labeledField:@"σクリップの閾値（σ）" field:_sigmaField] box:box];

    _lowMemoryCheck = [self checkboxWithTitle:@"低メモリモード（2GB上限）" state:NO];
    [self addToSection:key view:_lowMemoryCheck box:box];

    _rawCfaCheck = [self checkboxWithTitle:@"Bayerのまま合成する（デバイヤーしない）" state:NO];
    [_rawCfaCheck setTarget:self];
    [_rawCfaCheck setAction:@selector(inputInterpretationChanged:)];
    [_rawCfaCheck setToolTip:LSLocalizedString(@"PixInsight等で後からデバイヤーする場合に使います。品質評価からやり直しになります")];
    [self addToSection:key view:_rawCfaCheck box:box];
}

- (void)buildDrizzleSection:(NSStackView*)box {
    NSString* key = @"drizzle";
    [self beginSection:@"ドリズル拡大" key:key inBox:box];

    _drizzleSegment = [[[NSSegmentedControl alloc] init] autorelease];
    [_drizzleSegment setSegmentCount:kDrizzleChoiceCount];
    for (int i = 0; i < kDrizzleChoiceCount; ++i) {
        const double s = LSDrizzleScaleAt(i);
        [_drizzleSegment setLabel:(s == 1.5 ? @"1.5×" : [NSString stringWithFormat:@"%.0f×", s])
                       forSegment:i];
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
    [self addToSection:key
                  view:[self noteLabel:@"Drizzleが効くのは撮像がアンダーサンプリングの場合だけです。すでにオーバーサンプリング気味なら、倍率を上げても解像度は上がらず、ファイルサイズと処理時間だけが増えます。"]
                   box:box];
}

// ---- 仕上げ・出力タブ ------------------------------------------------------

// 仕上げの効果あり/なしの比較。どの仕上げ欄を開いていても見えるよう、タブの先頭に置く。
- (void)buildCompareSection:(NSStackView*)box {
    NSString* key = @"compare";
    [self beginSection:@"プレビューの比較" key:key inBox:box];
    _waveletPreviewCheck = [self checkboxWithTitle:@"仕上げ全体の効果をプレビュー" state:YES];
    [_waveletPreviewCheck setTarget:self];
    [_waveletPreviewCheck setAction:@selector(waveletPreviewChanged:)];
    [self addToSection:key view:_waveletPreviewCheck box:box];
    [self addToSection:key
                  view:[self noteLabel:@"OFFでスタックそのままを表示します。ON/OFFは比較表示のみで、書き出しには設定中の仕上げをすべて適用します。"]
                   box:box];
}

- (void)buildChannelSection:(NSStackView*)box {
    NSString* key = @"channel";
    [self beginSection:@"RGBチャンネル合わせ（大気分散）" key:key inBox:box];

    NSString* names[4] = {@"R x", @"R y", @"B x", @"B y"};
    for (int i = 0; i < 4; ++i) {
        _channelFields[i] = [self numberFieldWithValue:@"0.00" action:@selector(finishingChanged:)];
    }
    for (int r = 0; r < 2; ++r) {
        NSMutableArray* items = [NSMutableArray array];
        for (int k = 0; k < 2; ++k) {
            NSTextField* label = MakeLabel(names[r * 2 + k]);
            [items addObject:label];
            [items addObject:_channelFields[r * 2 + k]];
        }
        [self addToSection:key view:[self buttonRow:items] box:box];
    }
    [self addToSection:key
                  view:[self buttonRow:@[ [self buttonWithTitle:@"自動で合わせる" action:@selector(autoChannelAlign:)],
                                          [self buttonWithTitle:@"戻す" action:@selector(resetChannelAlign:)] ]]
                   box:box];
    [self addToSection:key
                  view:[self noteLabel:@"惑星の縁の赤・青のにじみ（大気の分散）を、R・BをGに重ねて取り除きます。単位は画素です。"]
                   box:box];
}

- (void)buildWaveletSection:(NSStackView*)box {
    NSString* key = @"wavelet";
    [self beginSection:@"ウェーブレット仕上げ" key:key inBox:box];

    // ウェーブレットだけの効果あり/なし（色・向きなどほかの仕上げは掛けたまま比べる）。
    _waveletOnlyPreviewCheck = [self checkboxWithTitle:@"ウェーブレットの効果をプレビュー" state:YES];
    [_waveletOnlyPreviewCheck setTarget:self];
    [_waveletOnlyPreviewCheck setAction:@selector(waveletOnlyPreviewChanged:)];
    [_waveletOnlyPreviewCheck setToolTip:LSLocalizedString(@"OFFでウェーブレットを掛ける前の画像を表示します（書き出しには設定中の効果を適用します）")];
    [self addToSection:key view:_waveletOnlyPreviewCheck box:box];

    [self addToSection:key
                  view:[self noteLabel:@"強調：1.00＝変化なし、8以上は強め ／ ノイズ：大きいほど強く低減"]
                   box:box];

    [self addToSection:key
                  view:[self noteLabel:@"－/＋で細かく調整できます（押し続けると連続して変わります）。数値欄に直接入力もできます。"]
                   box:box];

    for (int j = 0; j < kWaveletLayers; ++j) {
        NSString* title = [NSString stringWithFormat:LSLocalizedString(@"Layer %d（約%d px）"),
                                                     j + 1, 1 << (j + 1)];
        NSTextField* layerLabel = MakeLabel(title);
        [layerLabel setFont:[NSFont boldSystemFontOfSize:11.0]];
        // レイヤーごとの「初期値に戻す」（強調1.00・ノイズ0.00）。
        _layerResetButtons[j] = [self buttonWithTitle:@"初期値に戻す" action:@selector(resetLayer:)];
        [_layerResetButtons[j] setControlSize:NSControlSizeSmall];
        [_layerResetButtons[j] setFont:[NSFont systemFontOfSize:10.0]];
        [_layerResetButtons[j] setTag:j];
        [_layerResetButtons[j] setToolTip:LSLocalizedString(@"このレイヤーを強調1.00・ノイズ0.00に戻します")];
        NSStackView* header = [[[NSStackView alloc] init] autorelease];
        [header setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
        [header setTranslatesAutoresizingMaskIntoConstraints:NO];
        [header addView:layerLabel inGravity:NSStackViewGravityLeading];
        [header addView:_layerResetButtons[j] inGravity:NSStackViewGravityTrailing];
        [[header widthAnchor] constraintEqualToConstant:272.0].active = YES;
        [self addToSection:key view:header box:box];

        _sharpenSliders[j] = [self sliderMin:0.0 max:kWaveletGuiSharpenMaximum value:1.0
                                      action:@selector(waveletChanged:)];
        _sharpenValues[j] = [self valueFieldWithIndex:2 * j];
        [self addToSection:key
                      view:[self adjustRow:@"強調" slider:_sharpenSliders[j] field:_sharpenValues[j]
                                     index:2 * j step:0.05]
                       box:box];

        _denoiseSliders[j] = [self sliderMin:0.0 max:1.0 value:0.0
                                      action:@selector(waveletChanged:)];
        _denoiseValues[j] = [self valueFieldWithIndex:2 * j + 1];
        [self addToSection:key
                      view:[self adjustRow:@"ノイズ" slider:_denoiseSliders[j] field:_denoiseValues[j]
                                     index:2 * j + 1 step:0.01]
                       box:box];
    }

    // 連動（RegiStaxのLinked layers相当）。いまの配分のまま全体の強さだけを変える。
    _linkedCheck = [self checkboxWithTitle:@"レイヤーを連動（配分を保って強さを変える）" state:NO];
    [_linkedCheck setTarget:self];
    [_linkedCheck setAction:@selector(waveletChanged:)];
    [self addToSection:key view:_linkedCheck box:box];
    _linkedSlider = [self sliderMin:0.0 max:2.5 value:1.0 action:@selector(waveletChanged:)];
    _linkedValue = [self valueFieldWithIndex:kAdjustLinked];
    [self addToSection:key
                  view:[self adjustRow:@"強さ" slider:_linkedSlider field:_linkedValue index:kAdjustLinked step:0.02]
                   box:box];

    _deringSlider = [self sliderMin:0.0 max:1.0 value:0.0 action:@selector(finishingChanged:)];
    _deringValue = [self valueFieldWithIndex:kAdjustDering];
    [self addToSection:key
                  view:[self adjustRow:@"輪抑制" slider:_deringSlider field:_deringValue index:kAdjustDering step:0.01]
                   box:box];
    [self addToSection:key
                  view:[self noteLabel:@"輪抑制（デリンギング）：強調で明るい縁の外にできる暗い輪を抑えます。"]
                   box:box];

    [self addToSection:key
                  view:[self buttonRow:@[ [self buttonWithTitle:@"すべてリセット" action:@selector(resetWavelet:)] ]]
                   box:box];
}

- (void)buildColorSection:(NSStackView*)box {
    NSString* key = @"color";
    [self beginSection:@"色（ホワイトバランス・彩度）" key:key inBox:box];
    NSString* names[3] = {@"R", @"G", @"B"};
    for (int c = 0; c < 3; ++c) {
        _gainSliders[c] = [self sliderMin:0.5 max:2.0 value:1.0 action:@selector(finishingChanged:)];
        _gainValues[c] = MakeLabel(@"1.000");
        [self addToSection:key view:[self captionRow:names[c] slider:_gainSliders[c] value:_gainValues[c]] box:box];
    }
    _saturationSlider = [self sliderMin:0.0 max:2.0 value:1.0 action:@selector(finishingChanged:)];
    _saturationValue = MakeLabel(@"1.00");
    [self addToSection:key view:[self captionRow:@"彩度" slider:_saturationSlider value:_saturationValue] box:box];
    [self addToSection:key
                  view:[self buttonRow:@[ [self buttonWithTitle:@"自動（灰色仮説）" action:@selector(autoWhiteBalance:)],
                                          [self buttonWithTitle:@"戻す" action:@selector(resetColor:)] ]]
                   box:box];
}

- (void)buildToneSection:(NSStackView*)box {
    NSString* key = @"tone";
    [self beginSection:@"明るさ（黒点・白点・ガンマ）" key:key inBox:box];
    _toneCheck = [self checkboxWithTitle:@"ヒストグラムを調整する" state:NO];
    [_toneCheck setTarget:self];
    [_toneCheck setAction:@selector(finishingChanged:)];
    [self addToSection:key view:_toneCheck box:box];
    _blackSlider = [self sliderMin:0.0 max:0.5 value:0.0 action:@selector(finishingChanged:)];
    _blackValue = MakeLabel(@"0.000");
    [self addToSection:key view:[self captionRow:@"黒点" slider:_blackSlider value:_blackValue] box:box];
    _whiteSlider = [self sliderMin:0.05 max:1.0 value:1.0 action:@selector(finishingChanged:)];
    _whiteValue = MakeLabel(@"1.000");
    [self addToSection:key view:[self captionRow:@"白点" slider:_whiteSlider value:_whiteValue] box:box];
    _gammaSlider = [self sliderMin:0.3 max:3.0 value:1.0 action:@selector(finishingChanged:)];
    _gammaValue = MakeLabel(@"1.00");
    [self addToSection:key view:[self captionRow:@"ガンマ" slider:_gammaSlider value:_gammaValue] box:box];
    [self addToSection:key
                  view:[self buttonRow:@[ [self buttonWithTitle:@"自動で合わせる" action:@selector(autoTone:)] ]]
                   box:box];
    [self addToSection:key
                  view:[self noteLabel:@"ここでの調整は書き出す画像に入ります（プレビューの「表示を明るくする」は画面だけ）。"]
                   box:box];
}

- (void)buildGeometrySection:(NSStackView*)box {
    NSString* key = @"geometry";
    [self beginSection:@"向き・切り抜き" key:key inBox:box];
    _rotationLabel = MakeLabel(@"回転なし");
    [self addToSection:key
                  view:[self buttonRow:@[ [self buttonWithTitle:@"↺ 左へ90°" action:@selector(rotateLeft:)],
                                          [self buttonWithTitle:@"右へ90° ↻" action:@selector(rotateRight:)],
                                          _rotationLabel ]]
                   box:box];
    _flipHCheck = [self checkboxWithTitle:@"左右反転" state:NO];
    _flipVCheck = [self checkboxWithTitle:@"上下反転" state:NO];
    for (NSButton* b in @[ _flipHCheck, _flipVCheck ]) {
        [b setTarget:self];
        [b setAction:@selector(finishingChanged:)];
    }
    [self addToSection:key view:[self buttonRow:@[ _flipHCheck, _flipVCheck ]] box:box];

    _cropCheck = [self checkboxWithTitle:@"切り抜く" state:NO];
    [_cropCheck setTarget:self];
    [_cropCheck setAction:@selector(finishingChanged:)];
    _cropMarginField = [self numberFieldWithValue:@"16" action:@selector(autoCrop:)];
    [self addToSection:key
                  view:[self buttonRow:@[ _cropCheck, [self buttonWithTitle:@"対象を自動で囲む" action:@selector(autoCrop:)] ]]
                   box:box];
    [self addToSection:key view:[self labeledField:@"余白（px）" field:_cropMarginField] box:box];
    _cropLabel = [self noteLabel:@"切り抜く範囲はまだありません"];
    [self addToSection:key view:_cropLabel box:box];
    [self addToSection:key
                  view:[self buttonRow:@[ [self buttonWithTitle:@"切り抜きをやめる" action:@selector(clearCrop:)] ]]
                   box:box];
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

    [self addToSection:key view:MakeLabel(@"ファイル名の付け方") box:box];
    _nameStylePopup = [[[NSPopUpButton alloc] init] autorelease];
    [_nameStylePopup addItemWithTitle:@"元の名前＋処理条件"];
    [_nameStylePopup addItemWithTitle:@"WinJUPOS形式（撮影時刻を先頭に）"];
    [_nameStylePopup setTranslatesAutoresizingMaskIntoConstraints:NO];
    [_nameStylePopup setTarget:self];
    [_nameStylePopup setAction:@selector(formatChanged:)];
    [self addToSection:key view:_nameStylePopup box:box];

    _objectField = [[[NSTextField alloc] init] autorelease];
    [_objectField setTranslatesAutoresizingMaskIntoConstraints:NO];
    [_objectField setPlaceholderString:LSLocalizedString(@"例: Jupiter")];
    [_objectField setFont:[NSFont systemFontOfSize:11.0]];
    [[_objectField widthAnchor] constraintEqualToConstant:120.0].active = YES;
    [_objectField setTarget:self];
    [_objectField setAction:@selector(formatChanged:)];
    [[_objectField cell] setSendsActionOnEndEditing:YES];
    [self addToSection:key view:[self labeledField:@"対象名（ファイル・メタデータ用）" field:_objectField] box:box];

    _metadataCheck = [self checkboxWithTitle:@"処理条件と撮影時刻をファイルに記録する" state:YES];
    [self addToSection:key view:_metadataCheck box:box];

    [self addToSection:key view:MakeLabel(@"ファイル名") box:box];
    _namePreview = MakeLabel(@"");
    [_namePreview setTextColor:[NSColor secondaryLabelColor]];
    [_namePreview setLineBreakMode:NSLineBreakByTruncatingMiddle];
    [self addToSection:key view:_namePreview box:box];

    _saveButton = [self buttonWithTitle:@"名前を付けて書き出し…" action:@selector(save:)];
    [_saveButton setEnabled:NO];
    [self addToSection:key view:_saveButton box:box];

    // 採用率だけを変えた複数の結果を一度に書き出す（解析はやり直さない）。
    [self addToSection:key view:MakeLabel(@"複数の採用率で書き出す（% または枚数をカンマ区切り）") box:box];
    _multiPercentField = [[[NSTextField alloc] init] autorelease];
    [_multiPercentField setTranslatesAutoresizingMaskIntoConstraints:NO];
    [_multiPercentField setStringValue:@"5, 10, 25"];
    [_multiPercentField setFont:[NSFont systemFontOfSize:11.0]];
    [[_multiPercentField widthAnchor] constraintEqualToConstant:110.0].active = YES;
    _multiExportButton = [self buttonWithTitle:@"まとめて書き出し…" action:@selector(exportMultiplePercents:)];
    [self addToSection:key view:[self buttonRow:@[ _multiPercentField, _multiExportButton ]] box:box];
    [self addToSection:key
                  view:[self noteLabel:@"アライメント結果を使って採用率ごとにスタックし直し、いまの仕上げを掛けて選んだフォルダへ保存します。"]
                   box:box];
}

// --- 下部ステータスバー ---

- (NSView*)buildStatusBar {
    NSView* bar = [[[NSView alloc] initWithFrame:NSZeroRect] autorelease];

    _qualityButton = [self buttonWithTitle:@"品質評価" action:@selector(analyze:)];
    _alignButton = [self buttonWithTitle:@"アライメント" action:@selector(align:)];
    _stackButton = [self buttonWithTitle:@"スタック" action:@selector(run:)];
    [_stackButton setKeyEquivalent:@"\r"];
    _exportButton = [self buttonWithTitle:@"書き出し…" action:@selector(save:)];
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
    [_statusLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                           forOrientation:NSLayoutConstraintOrientationHorizontal];

    for (NSView* v in @[ _qualityButton, _alignButton, _stackButton, _exportButton, _cancelButton,
                         _progress, _statusLabel ]) {
        [bar addSubview:v];
    }

    NSDictionary* views = NSDictionaryOfVariableBindings(_qualityButton, _alignButton,
                                                         _stackButton, _exportButton,
                                                         _cancelButton, _progress, _statusLabel);
    [bar addConstraints:[NSLayoutConstraint
                            constraintsWithVisualFormat:
                                @"H:|[_qualityButton(>=76)]-6-[_alignButton(>=96)]-6-[_stackButton(>=76)]-6-"
                                @"[_exportButton(>=76)]-12-[_progress(>=100)]-10-[_statusLabel(>=120)]-8-"
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

// 数値の入力欄。Return または入力欄を離れたときに action を送る。
- (NSTextField*)numberFieldWithValue:(NSString*)value action:(SEL)action {
    NSTextField* field = [[[NSTextField alloc] init] autorelease];
    [field setStringValue:value];
    [field setFont:[NSFont systemFontOfSize:11.0]];
    [field setAlignment:NSTextAlignmentRight];
    [field setTranslatesAutoresizingMaskIntoConstraints:NO];
    [[field widthAnchor] constraintEqualToConstant:64.0].active = YES;
    [field setTarget:self];
    [field setAction:action];
    [[field cell] setSendsActionOnEndEditing:YES];
    return field;
}

- (NSView*)row:(NSView*)main trailing:(NSView*)trailing {
    NSStackView* row = [[[NSStackView alloc] init] autorelease];
    [row setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
    [row setSpacing:6.0];
    [row setTranslatesAutoresizingMaskIntoConstraints:NO];
    [row addArrangedSubview:main];
    [row addArrangedSubview:trailing];
    if ([trailing isKindOfClass:[NSTextField class]] && ![(NSTextField*)trailing isEditable]) {
        [[trailing widthAnchor] constraintEqualToConstant:46.0].active = YES;
    }
    return row;
}

@end
