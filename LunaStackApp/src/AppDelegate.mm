#import "AppDelegate.h"

#import "Localization.h"
#import "MainWindowController.h"

@interface AppDelegate () <NSMenuDelegate>
@end

@implementation AppDelegate {
    MainWindowController* _controller;
    // ウインドウができる前に Finder から渡されたファイル（ダブルクリックでの起動など）。
    NSMutableArray* _pendingPaths;
    NSMenu* _recentMenu;
    BOOL _selfTestFailed;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _pendingPaths = [[NSMutableArray alloc] init];
        _selfTestFailed = NO;
    }
    return self;
}

- (void)dealloc {
    [_pendingPaths release];
    [_controller release];
    [super dealloc];
}

- (void)check:(BOOL)ok {
    if (!ok) _selfTestFailed = YES;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    [self buildMenu];
    _controller = [[MainWindowController alloc] init];
    [_controller showWindow:nil];
    [[_controller window] makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    // 環境変数 LUNASTACK_SNAPSHOT が指すパスに、ウィンドウの中身をPNGで書き出して終了する。
    //
    // 画面キャプチャに頼らず画面構成を確認するための仕掛け。
    // ウィンドウが別のSpaceに出ていたり、CIのようにGUIセッションが無い環境でも、
    // レイアウトが壊れていないことを機械的に確かめられる。
    const char* snapshotPath = getenv("LUNASTACK_SNAPSHOT");
    const char* openPath = getenv("LUNASTACK_OPEN");
    const char* autorun = getenv("LUNASTACK_AUTORUN");
    const char* limitText = getenv("LUNASTACK_LIMIT");
    // "run"（既定、自己検証の一括処理）/ "staged"（GUIの3工程を順に通す）/
    // "analyze"（品質評価だけ）/ "alignment"（アライメントまで）
    const char* mode = getenv("LUNASTACK_MODE");
    // "幅x高さ"。最小サイズ（UI設計書 §2 の 1000×640）でも
    // 画面が崩れないことを機械的に確かめるために使う。
    const char* sizeText = getenv("LUNASTACK_SIZE");

    if (sizeText) {
        int w = 0, h = 0;
        if (sscanf(sizeText, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            NSWindow* window = [_controller window];
            NSRect frame = [window frame];
            frame.size = [window frameRectForContentRect:NSMakeRect(0, 0, w, h)].size;
            [window setFrame:frame display:YES];
            [window center];
        }
    }

    // 起動するたびに入力キュー・設定・仕上げはすべて初期状態から始める。
    // 以前の版（0.3.0の初期）が起動間に保存していた値は、残っていても使わずに消す。
    for (NSString* key in @[ @"lastSettings", @"queueItems", @"darkPath", @"flatPath" ]) {
        [[NSUserDefaults standardUserDefaults] removeObjectForKey:key];
    }

    // **フレーム数の制限はファイルを開く前に設定する。**
    // 開いた時点でサイドカーの照合が走り、そこには制限値も含まれる。
    // 順番が逆だと、同じ設定で解析した結果を「別物」と判断してしまう。
    if (limitText) {
        [_controller setFrameLimit:atoi(limitText)];
    }
    if (openPath) {
        // ":" 区切りで複数指定できる。
        NSString* joined = [NSString stringWithUTF8String:openPath];
        NSArray* paths = [joined componentsSeparatedByString:@":"];
        [_controller addPathsToQueue:paths];
    }
    if ([_pendingPaths count] > 0) {
        [_controller addPathsToQueue:_pendingPaths];
        [_controller openFileAtPath:[_pendingPaths lastObject]];
        [_pendingPaths removeAllObjects];
    }
    if (getenv("LUNASTACK_DARK") || getenv("LUNASTACK_FLAT")) {
        const char* dark = getenv("LUNASTACK_DARK");
        const char* flat = getenv("LUNASTACK_FLAT");
        [_controller setCalibrationForTestingDark:dark ? [NSString stringWithUTF8String:dark] : nil
                                             flat:flat ? [NSString stringWithUTF8String:flat] : nil];
    }
    if (const char* drizzle = getenv("LUNASTACK_DRIZZLE")) {
        [_controller setDrizzleIndexForTesting:atoi(drizzle)];
    }
    if (const char* zoom = getenv("LUNASTACK_ZOOM")) {
        [_controller setZoomIndexForTesting:atoi(zoom)];
    }
    // 処理範囲 "x:y:w:h"（入力の画素座標）または "center"（中央の半分）。
    if (const char* roi = getenv("LUNASTACK_ROI")) {
        [_controller setRoiForTesting:[NSString stringWithUTF8String:roi]];
    }

    if (autorun && snapshotPath) {
        // 実行が終わった時点で撮る。GUIを人が操作しなくても
        // 「開く→実行→プレビュー」の経路が通ることを確かめられる。
        NSString* path = [NSString stringWithUTF8String:snapshotPath];
        const char* sharpenText = getenv("LUNASTACK_SHARPEN");
        const char* finishText = getenv("LUNASTACK_FINISH");
        const char* apCheck = getenv("LUNASTACK_APCHECK");
        const BOOL selfTest = getenv("LUNASTACK_SELFTEST") != NULL;
        const BOOL stagedMode = mode && strcmp(mode, "staged") == 0;
        const BOOL alignmentMode = mode && strcmp(mode, "alignment") == 0;
        __block int stagedStep = 0;
        MainWindowController* controller = _controller;
        [_controller setOnRunFinished:^{
            if (selfTest && stagedStep == 0) {
                // 品質評価の直後に、スライダーの品質順を確かめる。
                [self check:[controller selfCheckFrameOrder]];
                [self check:[controller selfCheckFrameStepButtons]];
            }
            if (selfTest && stagedStep == 1) {
                // アライメントの後（除外フレームが並びの最後に来る状態）でも確かめる。
                [self check:[controller selfCheckFrameOrder]];
                [self check:[controller selfCheckDrizzleDiagnosis]];
            }
            if ((stagedMode && stagedStep < 2) || (alignmentMode && stagedStep < 1)) {
                ++stagedStep;
                if (stagedStep == 1) [controller startAlignmentOnly];
                else [controller startStackOnly];
                return;
            }
            if (selfTest) [self check:[controller selfCheckApHiddenAfterStack]];
            if (apCheck || selfTest) {
                // 描画とクリックの座標変換が食い違っていないかを見る。
                [self check:[controller selfCheckApHitTest]];
                [self check:[controller selfCheckEditingAndPresets]];
            }
            if (sharpenText) {
                // スライダーを動かしたのと同じ経路を通してから撮る。
                [controller setSharpenForTesting:atof(sharpenText) denoise:0.3];
            }
            if (finishText) {
                [controller setFinishingForTesting:[NSString stringWithUTF8String:finishText]];
            }
            if (selfTest) [self check:[controller selfCheckFinishingMatchesExport]];
            if (selfTest) [self check:[controller selfCheckWaveletPreviewToggle]];
            if (selfTest) [self check:[controller selfCheckContinuousPreview]];
            if (selfTest) [self check:[controller selfCheckWaveletControls]];
            if (selfTest) [self check:[controller selfCheckInspectorScrollsVerticallyOnly]];
            if (selfTest) [self check:[controller selfCheckMappingWaitsForResult]];
            if (selfTest) [self check:[controller selfCheckCropApplies]];
            if (selfTest) [self check:[controller selfCheckRoi]];
            if (selfTest) [self check:[controller selfCheckUntouchedInputKeepsResult]];
            if (getenv("LUNASTACK_WAVELET_OFF")) {
                [controller setWaveletPreviewForTesting:NO];
            }
            if (getenv("LUNASTACK_HEATMAP")) {
                [controller setApHeatmapForTesting:YES];
            }
            if (getenv("LUNASTACK_CLEAR_AFTER_RUN")) {
                [controller clearForTesting];
                // クリア後も同じ起動中に次の素材を追加できることを確認する。
                if (const char* reopenPath = getenv("LUNASTACK_REOPEN_AFTER_CLEAR")) {
                    [controller openFileAtPath:[NSString stringWithUTF8String:reopenPath]];
                }
            }
            // スナップショットに診断の結果を写す。
            if (getenv("LUNASTACK_DZDIAG")) [controller diagnoseDrizzleForTesting];
            [controller waitForFinishingForTesting];
            if (const char* tab = getenv("LUNASTACK_TAB")) [controller selectInspectorTabForTesting:atoi(tab)];
            if (const char* graph = getenv("LUNASTACK_GRAPH")) [controller setGraphModeForTesting:atoi(graph)];
            if (const char* pos = getenv("LUNASTACK_FRAMEPOS")) [controller showFrameAtSliderPositionForTesting:atoi(pos)];
            if (const char* sec = getenv("LUNASTACK_SECTION")) [controller scrollToSectionForTesting:[NSString stringWithUTF8String:sec]];
            if (const char* box = getenv("LUNASTACK_CROPBOX")) [controller showCropBoxForTesting:[NSString stringWithUTF8String:box]];
            [self writeSnapshotTo:path];
            if (selfTest) {
                // 最後に［クリア］が設定まで初期値に戻すことを確かめる（画面は撮ったあと）。
                [self check:[controller selfCheckClearResetsEverything]];
                NSLog(@"GUI自己検証: %@", _selfTestFailed ? @"失敗" : @"合格");
                exit(_selfTestFailed ? 1 : 0);
            }
            [NSApp terminate:nil];
        }];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
                           if (stagedMode || alignmentMode ||
                               (mode && strcmp(mode, "analyze") == 0)) {
                               [controller startAnalyzeOnly];
                           } else {
                               [controller startRun];
                           }
                       });
    } else if (snapshotPath) {
        NSString* path = [NSString stringWithUTF8String:snapshotPath];
        // レイアウトと描画が済むのを待ってから撮る。
        if (const char* tab = getenv("LUNASTACK_TAB")) [_controller selectInspectorTabForTesting:atoi(tab)];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
                           [self writeSnapshotTo:path];
                           [NSApp terminate:nil];
                       });
    }
}

- (void)writeSnapshotTo:(NSString*)path {
    NSView* view = [[_controller window] contentView];
    [view setNeedsDisplay:YES];
    [view displayIfNeeded];
    // 説明書用: 部品の位置（LUNASTACK_LAYOUT）と、設定パネルの縦に全部つないだ画像（LUNASTACK_INSPECTOR_SHOT）。
    if (const char* layout = getenv("LUNASTACK_LAYOUT")) {
        [_controller writeLayoutForTestingTo:[NSString stringWithUTF8String:layout]];
    }
    if (const char* inspector = getenv("LUNASTACK_INSPECTOR_SHOT")) {
        [_controller writeInspectorShotForTestingTo:[NSString stringWithUTF8String:inspector]];
    }

    NSBitmapImageRep* rep = [view bitmapImageRepForCachingDisplayInRect:[view bounds]];
    if (!rep) {
        NSLog(@"スナップショットを作れませんでした");
        return;
    }
    [view cacheDisplayInRect:[view bounds] toBitmapImageRep:rep];
    NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    if ([png writeToFile:path atomically:YES]) {
        NSLog(@"スナップショットを書き出しました: %@ (%.0fx%.0f)", path,
              [view bounds].size.width, [view bounds].size.height);
    } else {
        NSLog(@"スナップショットを保存できませんでした: %@", path);
    }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return YES;
}

// ⌘Q。処理中なら確認し、中断が済んでから終わる（B5）。
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    if (!_controller || getenv("LUNASTACK_SNAPSHOT")) return NSTerminateNow;
    return [_controller applicationShouldTerminate];
}

// Finderからのダブルクリック・Dockへのドロップ。
// ウインドウより先に呼ばれることがあるので、そのときは溜めておいて起動後に開く（B7）。
- (BOOL)application:(NSApplication*)sender openFile:(NSString*)filename {
    (void)sender;
    if (!_controller) {
        [_pendingPaths addObject:filename];
        return YES;
    }
    [_controller openFileAtPath:filename];
    return YES;
}

- (void)application:(NSApplication*)sender openFiles:(NSArray*)filenames {
    if (!_controller) {
        [_pendingPaths addObjectsFromArray:filenames];
    } else {
        [_controller addPathsToQueue:filenames];
        [_controller openFileAtPath:[filenames lastObject]];
    }
    [sender replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
}

// ---- メニュー --------------------------------------------------------------

- (NSMenuItem*)addItem:(NSMenu*)menu title:(NSString*)title action:(SEL)action key:(NSString*)key {
    NSMenuItem* item = [menu addItemWithTitle:LSLocalizedString(title) action:action keyEquivalent:key];
    return item;
}

// 最近使った項目は開くたびに作り直す。
- (void)menuNeedsUpdate:(NSMenu*)menu {
    if (menu != _recentMenu) return;
    [menu removeAllItems];
    NSArray* paths = _controller ? [_controller recentPaths] : @[];
    for (NSString* path in paths) {
        NSMenuItem* item = [menu addItemWithTitle:[path lastPathComponent]
                                           action:@selector(openRecent:)
                                    keyEquivalent:@""];
        [item setRepresentedObject:path];
        [item setToolTip:path];
    }
    if ([paths count] == 0) {
        NSMenuItem* empty = [menu addItemWithTitle:LSLocalizedString(@"（なし）") action:NULL keyEquivalent:@""];
        [empty setEnabled:NO];
    }
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItemWithTitle:LSLocalizedString(@"履歴を消去") action:@selector(clearRecent:) keyEquivalent:@""];
}

// メニューバーを手で作る。nibを使わないため。
// 「開く」「終了」など最低限のショートカットが無いとアプリとして成立しない。
// 編集メニューが無いと、入力欄で ⌘C / ⌘V / ⌘A すら効かない（B8）。
- (void)buildMenu {
    NSMenu* mainMenu = [[[NSMenu alloc] init] autorelease];

    // アプリ
    NSMenuItem* appItem = [[[NSMenuItem alloc] init] autorelease];
    [mainMenu addItem:appItem];
    NSMenu* appMenu = [[[NSMenu alloc] init] autorelease];
    [self addItem:appMenu title:@"LunaStack について" action:@selector(orderFrontStandardAboutPanel:) key:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [self addItem:appMenu title:@"LunaStack を隠す" action:@selector(hide:) key:@"h"];
    NSMenuItem* hideOthers = [self addItem:appMenu title:@"ほかを隠す" action:@selector(hideOtherApplications:) key:@"h"];
    [hideOthers setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];
    [self addItem:appMenu title:@"すべてを表示" action:@selector(unhideAllApplications:) key:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [self addItem:appMenu title:@"LunaStack を終了" action:@selector(terminate:) key:@"q"];
    [appItem setSubmenu:appMenu];

    // ファイル
    NSMenuItem* fileItem = [[[NSMenuItem alloc] init] autorelease];
    [mainMenu addItem:fileItem];
    NSMenu* fileMenu = [[[NSMenu alloc] initWithTitle:LSLocalizedString(@"ファイル")] autorelease];
    [self addItem:fileMenu title:@"開く…" action:@selector(openDocument:) key:@"o"];
    NSMenuItem* recentItem = [self addItem:fileMenu title:@"最近使った項目" action:NULL key:@""];
    _recentMenu = [[[NSMenu alloc] initWithTitle:LSLocalizedString(@"最近使った項目")] autorelease];
    [_recentMenu setDelegate:self];
    [recentItem setSubmenu:_recentMenu];
    [fileMenu addItem:[NSMenuItem separatorItem]];
    [self addItem:fileMenu title:@"書き出し…" action:@selector(save:) key:@"s"];
    NSMenuItem* multi = [self addItem:fileMenu title:@"複数の採用率で書き出し…" action:@selector(exportMultiplePercents:) key:@"s"];
    [multi setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagShift];
    [fileMenu addItem:[NSMenuItem separatorItem]];
    [self addItem:fileMenu title:@"閉じる" action:@selector(performClose:) key:@"w"];
    [fileItem setSubmenu:fileMenu];

    // 編集（入力欄のコピー・ペーストと、位置合わせ領域の編集の取り消し）
    NSMenuItem* editItem = [[[NSMenuItem alloc] init] autorelease];
    [mainMenu addItem:editItem];
    NSMenu* editMenu = [[[NSMenu alloc] initWithTitle:LSLocalizedString(@"編集")] autorelease];
    [self addItem:editMenu title:@"取り消す" action:@selector(undo:) key:@"z"];
    NSMenuItem* redo = [self addItem:editMenu title:@"やり直す" action:@selector(redo:) key:@"z"];
    [redo setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagShift];
    [editMenu addItem:[NSMenuItem separatorItem]];
    [self addItem:editMenu title:@"カット" action:@selector(cut:) key:@"x"];
    [self addItem:editMenu title:@"コピー" action:@selector(copy:) key:@"c"];
    [self addItem:editMenu title:@"ペースト" action:@selector(paste:) key:@"v"];
    [self addItem:editMenu title:@"すべてを選択" action:@selector(selectAll:) key:@"a"];
    [editItem setSubmenu:editMenu];

    // 表示
    NSMenuItem* viewItem = [[[NSMenuItem alloc] init] autorelease];
    [mainMenu addItem:viewItem];
    NSMenu* viewMenu = [[[NSMenu alloc] initWithTitle:LSLocalizedString(@"表示")] autorelease];
    [self addItem:viewMenu title:@"入力キューと品質グラフ" action:@selector(toggleLeftPane:) key:@"1"];
    [self addItem:viewMenu title:@"設定（Inspector）" action:@selector(toggleRightPane:) key:@"2"];
    [viewMenu addItem:[NSMenuItem separatorItem]];
    [self addItem:viewMenu title:@"拡大" action:@selector(zoomIn:) key:@"+"];
    [self addItem:viewMenu title:@"縮小" action:@selector(zoomOut:) key:@"-"];
    [self addItem:viewMenu title:@"全体を表示" action:@selector(zoomToFit:) key:@"0"];
    [self addItem:viewMenu title:@"実画素で等倍" action:@selector(zoomActualPixels:) key:@"9"];
    [viewItem setSubmenu:viewMenu];

    // 処理
    NSMenuItem* processItem = [[[NSMenuItem alloc] init] autorelease];
    [mainMenu addItem:processItem];
    NSMenu* processMenu = [[[NSMenu alloc] initWithTitle:LSLocalizedString(@"処理")] autorelease];
    [self addItem:processMenu title:@"品質評価" action:@selector(analyze:) key:@"e"];
    // ⌘A は「すべてを選択」に譲る。
    NSMenuItem* align = [self addItem:processMenu title:@"アライメント" action:@selector(align:) key:@"a"];
    [align setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagShift];
    [self addItem:processMenu title:@"スタック" action:@selector(run:) key:@"r"];
    [processMenu addItem:[NSMenuItem separatorItem]];
    [self addItem:processMenu title:@"中断" action:@selector(cancel:) key:@"."];
    [processItem setSubmenu:processMenu];

    // ウインドウ
    NSMenuItem* windowItem = [[[NSMenuItem alloc] init] autorelease];
    [mainMenu addItem:windowItem];
    NSMenu* windowMenu =
        [[[NSMenu alloc] initWithTitle:LSLocalizedString(@"ウインドウ")] autorelease];
    [self addItem:windowMenu title:@"しまう" action:@selector(performMiniaturize:) key:@"m"];
    [self addItem:windowMenu title:@"拡大／縮小" action:@selector(performZoom:) key:@""];
    [windowItem setSubmenu:windowMenu];
    [NSApp setWindowsMenu:windowMenu];

    [NSApp setMainMenu:mainMenu];
}

@end
