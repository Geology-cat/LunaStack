#import "MainWindowController_Private.h"

#include <algorithm>
#include <cmath>

#include "stackcore/map_pipeline.hpp"
#include "stackcore/video_source.hpp"
#include "stackcore/wavelet.hpp"

@implementation MainWindowController (Queue)

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
    if (index != _currentIndex) _bannerDismissed = NO;
    _currentIndex = index;
    [_queueTable selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(index)]
             byExtendingSelection:NO];

    QueueItem* item = _items[static_cast<NSUInteger>(index)];
    _inputPath = std::string([[item path] UTF8String]);

    // 選んだファイルが変わったら、前のファイルの解析結果は使えない。
    _qualityStage.reset();
    _globalStage.reset();
    [_qualitySignature release];
    _qualitySignature = nil;
    [_globalSignature release];
    _globalSignature = nil;
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
        [self resetWorkspaceForNewProcessing];
        [_statusLabel setStringValue:LSLocalizedString(@"動画を追加してください")];
    }
    [self updateControlsEnabled];
}

// 次の素材をすぐに処理できる空の作業状態へ戻す。
// 元動画、サイドカー、書き出し済みファイルには触れず、画面内の状態だけを破棄する。
- (void)resetWorkspaceForNewProcessing {
    _currentIndex = -1;
    _inputPath.clear();

    _qualityStage.reset();
    _globalStage.reset();
    [_qualitySignature release];
    _qualitySignature = nil;
    [_globalSignature release];
    _globalSignature = nil;
    _analysis.reset();
    [_analysisSignature release];
    _analysisSignature = nil;
    _referenceImage.reset();
    _stacked.reset();
    _displayed.reset();
    _wavelet.reset();

    _manualPointsActive = NO;
    _manualPoints.clear();
    _sourceChannels = 1;
    _sourceFrames = 0;
    _sourceWidth = 0;
    _sourceHeight = 0;
    _rejectedFrames = 0;
    _byteOrderSuspect = NO;
    _looksLikeShallowDepth = NO;
    _bannerDismissed = NO;

    [_queueTable deselectAll:nil];
    [_graph clearData];
    [_preview clearImage];
    [_preview clearAlignmentPoints];
    [_apCountLabel setStringValue:@""];
    [_frameSlider setMinValue:0.0];
    [_frameSlider setMaxValue:0.0];
    [_frameSlider setDoubleValue:0.0];
    [_viewModeSegment setSelectedSegment:0];
    [_inspectorTab setSelectedSegment:0];
    [_progress setDoubleValue:0.0];
    [_progress setHidden:YES];
    [_statusLabel
        setStringValue:LSLocalizedString(@"クリアしました — 新しい動画を追加してください")];

    [self updateBanner];
    [self updateInspectorVisibility];
    [self updateNamePreview];
    [self updateDrizzleEstimate];
    [self updateControlsEnabled];
}

- (void)clearWorkspace:(id)sender {
    (void)sender;
    if (_running || [_items count] == 0) return;
    [_items removeAllObjects];
    [_queueTable reloadData];
    [self resetWorkspaceForNewProcessing];
}

- (void)clearForTesting {
    [self clearWorkspace:nil];
}

// 右クリックされた行を優先する。通常の選択行とは別の行を右クリックしても、
// 意図したファイルに操作が掛かるようにする（UI設計書 §3.1）。
- (NSInteger)contextQueueRow {
    const NSInteger clicked = [_queueTable clickedRow];
    return clicked >= 0 ? clicked : [_queueTable selectedRow];
}

- (void)menuWillOpen:(NSMenu*)menu {
    if (menu != [_queueTable menu] || _running) return;
    const NSInteger row = [self contextQueueRow];
    if (row >= 0 && row < static_cast<NSInteger>([_items count])) {
        if (row != _currentIndex) [self selectQueueIndex:row];
        else {
            [_queueTable
                selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(row)]
                byExtendingSelection:NO];
        }
    }
}

- (BOOL)validateMenuItem:(NSMenuItem*)menuItem {
    const SEL action = [menuItem action];
    if (action != @selector(revealQueueItemInFinder:) &&
        action != @selector(removeSelectedFromQueue:)) {
        return YES;
    }
    const NSInteger row = [self contextQueueRow];
    const BOOL hasRow = row >= 0 && row < static_cast<NSInteger>([_items count]);
    if (action == @selector(revealQueueItemInFinder:)) return hasRow;
    return hasRow && !_running;
}

- (void)revealQueueItemInFinder:(id)sender {
    (void)sender;
    const NSInteger row = [self contextQueueRow];
    if (row < 0 || row >= static_cast<NSInteger>([_items count])) return;
    NSString* path = [_items[static_cast<NSUInteger>(row)] path];
    [[NSWorkspace sharedWorkspace]
        activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:path] ]];
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

@end
