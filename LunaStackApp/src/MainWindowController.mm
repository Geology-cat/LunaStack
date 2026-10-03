#import "MainWindowController_Private.h"

@implementation MainWindowController

@synthesize onRunFinished = _onRunFinished;

- (instancetype)init {
    // 10.12でも使えるスタイルマスク定数を使う。
    const NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    NSWindow* window = [[[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 800)
                                                    styleMask:style
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO] autorelease];
    [window setTitle:@"LunaStack"];
    // UI設計書 §2 の最小サイズ。10.12・10.13世代のノートでも収まること。
    [window setMinSize:NSMakeSize(1000, 640)];
    [window center];

    self = [super initWithWindow:window];
    if (!self) return nil;

    _cancelFlag = new std::atomic<bool>(false);
    _running = NO;
    _terminateAfterCancel = NO;
    _closeAfterCancel = NO;
    _frameLimit = 0;
    _selectionUsesCount = NO;
    _apTopPercentSetting = 10.0;
    _apTopCountSetting = 100;
    _currentIndex = -1;
    _manualPointsActive = NO;
    _sourceChannels = 1;
    _sourceFrames = 0;
    _sourceTotalFrames = 0;
    _byteOrderSuspect = NO;
    _looksLikeShallowDepth = NO;
    _bannerDismissed = NO;
    _rejectedFrames = 0;
    _sourceWidth = 0;
    _sourceHeight = 0;
    _inputIsSequence = NO;
    _calibrationDirty = NO;
    _frameOrderByQuality = NO;
    _restoringSettings = NO;
    _rotationTurns = 0;
    _cropRect = NSZeroRect;
    _renderGeneration = 0;
    _etaStart = 0.0;
    _items = [[NSMutableArray alloc] init];
    _sections = [[NSMutableDictionary alloc] init];
    _sectionHeaders = [[NSMutableDictionary alloc] init];
    _etaStage = nil;
    _stackedInfo = nil;
    _darkPath = nil;
    _flatPath = nil;
    _finishQueue = dispatch_queue_create("com.lunastack.finishing", DISPATCH_QUEUE_SERIAL);

    [window setDelegate:self];
    [self buildInterface];
    [self reloadPresets];
    _defaultSettings = [[self settingsDictionary] retain];
    return self;
}

- (void)dealloc {
    delete _cancelFlag;
    [_items release];
    [_sections release];
    [_sectionHeaders release];
    [_qualitySignature release];
    [_globalSignature release];
    [_analysisSignature release];
    [_drizzleDiagnosisKey release];
    [_etaStage release];
    [_stackedInfo release];
    [_darkPath release];
    [_flatPath release];
    [_openedInputSignature release];
    [_defaultSettings release];
    [_onRunFinished release];
    dispatch_release(_finishQueue);
    [super dealloc];
}

// ---- 終了・中断の確認 -------------------------------------------------------

// 処理中なら中断してよいかを尋ねる。YES なら中断を始めている。
- (BOOL)confirmStopForReason:(NSString*)reason {
    NSAlert* alert = [[[NSAlert alloc] init] autorelease];
    [alert setMessageText:LSLocalizedString(@"処理中です")];
    [alert setInformativeText:reason];
    [alert addButtonWithTitle:LSLocalizedString(@"中断する")];
    [alert addButtonWithTitle:LSLocalizedString(@"処理を続ける")];
    if ([alert runModal] != NSAlertFirstButtonReturn) return NO;
    _cancelFlag->store(true);
    [_statusLabel setStringValue:LSLocalizedString(@"中断しています…")];
    return YES;
}

// 処理が止まったときに呼ぶ。閉じる・終了の予約があればここで実行する。
- (void)jobDidStop {
    if (_terminateAfterCancel) {
        _terminateAfterCancel = NO;
        [NSApp terminate:nil];
        return;
    }
    if (_closeAfterCancel) {
        _closeAfterCancel = NO;
        [[self window] close];
    }
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    if (_running) {
        // 中断が済むまで閉じない。済んだら jobDidStop が閉じる。
        if ([self confirmStopForReason:LSLocalizedString(
                                           @"ウインドウを閉じる前に処理を中断します。途中までの結果は保存されません")]) {
            _closeAfterCancel = YES;
        }
        return NO;
    }
    return YES;
}

// アプリの終了要求（⌘Q）。処理中なら確認し、中断が済んでから終了する。
- (NSApplicationTerminateReply)applicationShouldTerminate {
    if (!_running) {
        return NSTerminateNow;
    }
    if ([self confirmStopForReason:LSLocalizedString(
                                       @"終了する前に処理を中断します。途中までの結果は保存されません")]) {
        _terminateAfterCancel = YES;
    }
    return NSTerminateCancel;
}

@end
