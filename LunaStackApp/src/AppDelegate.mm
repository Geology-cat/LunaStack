#import "AppDelegate.h"

#import "MainWindowController.h"

@implementation AppDelegate {
    MainWindowController* _controller;
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
    // "run"（既定）/ "analyze"（解析だけ）/ "batch"（キューを一括処理）
    const char* mode = getenv("LUNASTACK_MODE");
    const char* outDir = getenv("LUNASTACK_OUTDIR");
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

    // **フレーム数の制限はファイルを開く前に設定する。**
    // 開いた時点でサイドカーの照合が走り、そこには制限値も含まれる。
    // 順番が逆だと、同じ設定で解析した結果を「別物」と判断してしまう。
    if (limitText) {
        [_controller setFrameLimit:atoi(limitText)];
    }
    if (openPath) {
        // ":" 区切りで複数指定できる。バッチの経路を人手なしで通すため。
        NSString* joined = [NSString stringWithUTF8String:openPath];
        NSArray* paths = [joined componentsSeparatedByString:@":"];
        [_controller addPathsToQueue:paths];
    }
    if (outDir) {
        [_controller setBatchOutputDirectory:[NSString stringWithUTF8String:outDir]];
    }
    if (const char* drizzle = getenv("LUNASTACK_DRIZZLE")) {
        [_controller setDrizzleIndexForTesting:atoi(drizzle)];
    }
    if (const char* zoom = getenv("LUNASTACK_ZOOM")) {
        [_controller setZoomIndexForTesting:atoi(zoom)];
    }

    if (autorun && snapshotPath) {
        // 実行が終わった時点で撮る。GUIを人が操作しなくても
        // 「開く→実行→プレビュー」の経路が通ることを確かめられる。
        NSString* path = [NSString stringWithUTF8String:snapshotPath];
        MainWindowController* controller = _controller;
        const char* sharpenText = getenv("LUNASTACK_SHARPEN");
        const char* apCheck = getenv("LUNASTACK_APCHECK");
        [_controller setOnRunFinished:^{
            if (apCheck) {
                // 描画とクリックの座標変換が食い違っていないかを見る。
                NSLog(@"AP当たり判定: %@", [_controller selfCheckApHitTest] ? @"一致" : @"ずれあり");
                [_controller selfCheckEditingAndPresets];
            }
            if (sharpenText) {
                // スライダーを動かしたのと同じ経路を通してから撮る。
                [_controller setSharpenForTesting:atof(sharpenText) denoise:0.3];
            }
            if (getenv("LUNASTACK_HEATMAP")) {
                [_controller setApHeatmapForTesting:YES];
            }
            [self writeSnapshotTo:path];
            [NSApp terminate:nil];
        }];
        (void)controller;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
                           if (mode && strcmp(mode, "analyze") == 0) {
                               [_controller startAnalyzeOnly];
                           } else if (mode && strcmp(mode, "batch") == 0) {
                               [_controller startBatch];
                           } else {
                               [_controller startRun];
                           }
                       });
    } else if (snapshotPath) {
        NSString* path = [NSString stringWithUTF8String:snapshotPath];
        // レイアウトと描画が済むのを待ってから撮る。
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

- (BOOL)application:(NSApplication*)sender openFile:(NSString*)filename {
    (void)sender;
    if (!_controller) return NO;
    [_controller openFileAtPath:filename];
    return YES;
}

// メニューバーを手で作る。nibを使わないため。
// 「開く」「終了」など最低限のショートカットが無いとアプリとして成立しない。
- (void)buildMenu {
    NSMenu* mainMenu = [[[NSMenu alloc] init] autorelease];

    NSMenuItem* appItem = [[[NSMenuItem alloc] init] autorelease];
    [mainMenu addItem:appItem];
    NSMenu* appMenu = [[[NSMenu alloc] init] autorelease];
    [appMenu addItemWithTitle:@"LunaStack について"
                       action:@selector(orderFrontStandardAboutPanel:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"LunaStack を隠す" action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"LunaStack を終了" action:@selector(terminate:) keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];

    NSMenuItem* fileItem = [[[NSMenuItem alloc] init] autorelease];
    [mainMenu addItem:fileItem];
    NSMenu* fileMenu = [[[NSMenu alloc] initWithTitle:@"ファイル"] autorelease];
    [fileMenu addItemWithTitle:@"開く…" action:@selector(openDocument:) keyEquivalent:@"o"];
    [fileMenu addItemWithTitle:@"書き出し…" action:@selector(save:) keyEquivalent:@"s"];
    [fileItem setSubmenu:fileMenu];

    NSMenuItem* processItem = [[[NSMenuItem alloc] init] autorelease];
    [mainMenu addItem:processItem];
    NSMenu* processMenu = [[[NSMenu alloc] initWithTitle:@"処理"] autorelease];
    [processMenu addItemWithTitle:@"解析" action:@selector(analyze:) keyEquivalent:@"e"];
    [processMenu addItemWithTitle:@"スタック" action:@selector(run:) keyEquivalent:@"r"];
    [processMenu addItemWithTitle:@"すべて処理" action:@selector(batch:) keyEquivalent:@"b"];
    [processMenu addItem:[NSMenuItem separatorItem]];
    [processMenu addItemWithTitle:@"中断" action:@selector(cancel:) keyEquivalent:@"."];
    [processItem setSubmenu:processMenu];

    NSMenuItem* windowItem = [[[NSMenuItem alloc] init] autorelease];
    [mainMenu addItem:windowItem];
    NSMenu* windowMenu = [[[NSMenu alloc] initWithTitle:@"ウインドウ"] autorelease];
    [windowMenu addItemWithTitle:@"しまう" action:@selector(performMiniaturize:)
                   keyEquivalent:@"m"];
    [windowItem setSubmenu:windowMenu];
    [NSApp setWindowsMenu:windowMenu];

    [NSApp setMainMenu:mainMenu];
}

@end
