#import "MainWindowController_Private.h"

#include <algorithm>
#include <cmath>

#include "stackcore/map_pipeline.hpp"
#include "stackcore/video_source.hpp"
#include "stackcore/wavelet.hpp"

@implementation MainWindowController

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
    _running = NO;
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
    [_items release];
    [_sections release];
    [_sectionHeaders release];
    [_qualitySignature release];
    [_globalSignature release];
    [_analysisSignature release];
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
        _cancelFlag->store(true);
        return NO;  // 中断が終わるまで閉じない
    }
    return YES;
}

@end
