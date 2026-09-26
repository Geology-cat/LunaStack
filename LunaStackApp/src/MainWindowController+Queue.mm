#import "MainWindowController_Private.h"

#include <algorithm>

#include "stackcore/image_reader.hpp"

namespace {

// 最近使った項目の上限。
constexpr NSUInteger kRecentLimit = 10;

BOOL IsVideoPath(NSString* path) {
    NSString* ext = [[path pathExtension] lowercaseString];
    return [ext isEqualToString:@"ser"] || [ext isEqualToString:@"avi"];
}

BOOL IsImagePath(NSString* path) {
    return stackcore::is_supported_image_path(std::string([path UTF8String])) ? YES : NO;
}

// 自然順（"img2" < "img10"）。エンジンの連番と同じ並べ方にする。
NSComparisonResult NaturalCompare(NSString* a, NSString* b) {
    const std::string sa([a UTF8String]), sb([b UTF8String]);
    if (stackcore::natural_less(sa, sb)) return NSOrderedAscending;
    if (stackcore::natural_less(sb, sa)) return NSOrderedDescending;
    return NSOrderedSame;
}

}  // namespace

@implementation MainWindowController (Queue)

// ---- 表示 -----------------------------------------------------------------

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
    [field setToolTip:[item isSequence] && [[item sequenceFiles] count] > 0
                          ? [[item sequenceFiles] componentsJoinedByString:@"\n"]
                          : [item path]];

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
                  initWithString:[item displayName]
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

// 1行だけ描き直す（選択を保ったまま）。
- (void)reloadQueueRow:(NSInteger)row {
    if (row < 0 || row >= static_cast<NSInteger>([_items count])) return;
    [_queueTable reloadDataForRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(row)]
                           columnIndexes:[NSIndexSet indexSetWithIndex:0]];
}

- (void)tableViewSelectionDidChange:(NSNotification*)notification {
    (void)notification;
    // setEnabled:NO でも選択が動く経路があるので、ここでも実行中は入力を切り替えない。
    if (_running) return;
    const NSInteger row = [_queueTable selectedRow];
    if (row < 0 || row == _currentIndex) return;
    [self selectQueueIndex:row];
}

// ---- ドラッグ&ドロップ ------------------------------------------------------

// 落とせるものが1つでもあるか。無ければ受け付けない（落としても何も起きないのが分かる）。
- (BOOL)pasteboardHasUsableFiles:(NSPasteboard*)pasteboard {
    NSArray* urls = [pasteboard readObjectsForClasses:@[ [NSURL class] ]
                                              options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    NSFileManager* fm = [NSFileManager defaultManager];
    for (NSURL* url in urls) {
        BOOL isDir = NO;
        if (![fm fileExistsAtPath:[url path] isDirectory:&isDir]) continue;
        if (isDir || IsVideoPath([url path]) || IsImagePath([url path])) return YES;
    }
    return NO;
}

- (NSDragOperation)tableView:(NSTableView*)tableView
                validateDrop:(id<NSDraggingInfo>)info
                 proposedRow:(NSInteger)row
       proposedDropOperation:(NSTableViewDropOperation)op {
    (void)tableView;
    (void)row;
    (void)op;
    if (_running || ![self pasteboardHasUsableFiles:[info draggingPasteboard]]) {
        return NSDragOperationNone;
    }
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

// ---- 追加 -----------------------------------------------------------------

// フォルダを調べる。動画があればそれぞれを、画像が直下にあれば連番1本として加える。
// 下のフォルダも同じ規則で調べる（PIPPなどは連番をフォルダごとに書き出す）。
- (void)collectFromDirectory:(NSString*)dir into:(NSMutableArray*)items depth:(int)depth {
    if (depth > 6) return;  // シンボリックリンクの循環などで止まらなくならないように
    NSFileManager* fm = [NSFileManager defaultManager];
    NSArray* names = [[fm contentsOfDirectoryAtPath:dir error:NULL]
        sortedArrayUsingComparator:^NSComparisonResult(id a, id b) { return NaturalCompare(a, b); }];
    BOOL hasImages = NO;
    for (NSString* name in names) {
        if ([name hasPrefix:@"."]) continue;
        NSString* full = [dir stringByAppendingPathComponent:name];
        BOOL isDir = NO;
        if (![fm fileExistsAtPath:full isDirectory:&isDir]) continue;
        if (isDir) {
            [self collectFromDirectory:full into:items depth:depth + 1];
        } else if (IsVideoPath(full)) {
            [items addObject:[QueueItem itemWithPath:full]];
        } else if (IsImagePath(full)) {
            hasImages = YES;
        }
    }
    if (hasImages) [items addObject:[QueueItem sequenceItemWithDirectory:dir files:@[]]];
}

- (void)addPathsToQueue:(NSArray*)paths {
    NSFileManager* fm = [NSFileManager defaultManager];
    NSMutableArray* candidates = [NSMutableArray array];
    // 個別に選んだ画像は、フォルダごとに1本の連番にまとめる。
    NSMutableDictionary* imagesByDir = [NSMutableDictionary dictionary];
    NSMutableArray* dirOrder = [NSMutableArray array];
    int skipped = 0;
    int unified = 0;  // 連番の形式をそろえるために外した画像

    for (NSString* path in paths) {
        BOOL isDir = NO;
        if (![fm fileExistsAtPath:path isDirectory:&isDir]) continue;
        if (isDir) {
            [self collectFromDirectory:path into:candidates depth:0];
        } else if (IsVideoPath(path)) {
            [candidates addObject:[QueueItem itemWithPath:path]];
        } else if (IsImagePath(path)) {
            NSString* dir = [path stringByDeletingLastPathComponent];
            if (!imagesByDir[dir]) {
                imagesByDir[dir] = [NSMutableArray array];
                [dirOrder addObject:dir];
            }
            [imagesByDir[dir] addObject:path];
        } else {
            ++skipped;
        }
    }
    for (NSString* dir in dirOrder) {
        NSArray* sorted = [imagesByDir[dir] sortedArrayUsingComparator:^NSComparisonResult(id a, id b) {
            return NaturalCompare([a lastPathComponent], [b lastPathComponent]);
        }];
        // RAW と JPEG を一緒に選んだときなどは1種類にそろえる（混ぜると途中で形式が食い違う）。
        std::vector<std::string> chosen;
        for (NSString* f in sorted) chosen.push_back([f UTF8String]);
        chosen = stackcore::select_sequence_files(chosen);
        NSMutableArray* files = [NSMutableArray array];
        for (const std::string& f : chosen) [files addObject:[NSString stringWithUTF8String:f.c_str()]];
        unified += static_cast<int>([sorted count] - [files count]);
        [candidates addObject:[QueueItem sequenceItemWithDirectory:dir files:files]];
    }

    NSInteger firstAdded = -1;
    for (QueueItem* item in candidates) {
        BOOL duplicate = NO;
        for (QueueItem* existing in _items) {
            if ([existing isSameInputAs:item]) duplicate = YES;
        }
        if (duplicate) continue;
        [self fillHeaderInfo:item];
        [_items addObject:item];
        if (firstAdded < 0) firstAdded = static_cast<NSInteger>([_items count]) - 1;
        [self noteRecentPath:[item isSequence] && [[item sequenceFiles] count] == 0 ? [item path]
                             : ([item isSequence] ? nil : [item path])];
    }
    [_queueTable reloadData];
    if (_currentIndex >= 0 && _currentIndex < static_cast<NSInteger>([_items count])) {
        [_queueTable selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(_currentIndex)]
                 byExtendingSelection:NO];
    }
    if ([_items count] > 0 && _currentIndex < 0) {
        [self selectQueueIndex:firstAdded >= 0 ? firstAdded : 0];
    }
    if (skipped > 0) {
        [_statusLabel setStringValue:[NSString stringWithFormat:
                                                   LSLocalizedString(@"対応していないファイルを %d 件除外しました（SER・AVI・静止画のみ）"),
                                                   skipped]];
    } else if (unified > 0) {
        [_statusLabel setStringValue:[NSString stringWithFormat:
                                                   LSLocalizedString(@"連番の形式をそろえるため %d 枚を除外しました（RAWがあればRAWだけを使います）"),
                                                   unified]];
    }
    [self updateControlsEnabled];
}

// ヘッダだけ読んで副題を埋める。中身の妥当性はここで分かる。
- (void)fillHeaderInfo:(QueueItem*)item {
    try {
        stackcore::OpenOptions options;
        options.endian = [self currentOpenOptions].endian;
        options.bit_depth_override = [self currentOpenOptions].bit_depth_override;
        for (NSString* f in [item sequenceFiles]) options.sequence_files.push_back([f UTF8String]);
        const std::unique_ptr<stackcore::VideoSource> source =
            stackcore::open_video(std::string([[item path] UTF8String]), options);
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
        QueueItem* item = _items[i];
        if ([[item path] isEqualToString:path] ||
            [[item sequenceFiles] containsObject:path]) {
            if (!_running) [self selectQueueIndex:static_cast<NSInteger>(i)];
            break;
        }
    }
}

// ---- 選択 -----------------------------------------------------------------

- (NSString*)inputPathString {
    // パスに日本語が入りうるので、常に UTF-8 として変換する。
    return [NSString stringWithUTF8String:_inputPath.c_str()];
}

- (NSString*)inputDisplayName {
    if (_currentIndex < 0 || _currentIndex >= static_cast<NSInteger>([_items count])) return @"";
    return [_items[static_cast<NSUInteger>(_currentIndex)] displayName];
}

// 入力の大きさ（サイドカーの照合用）。連番は全ファイルの合計。
- (long long)inputSizeBytes {
    NSFileManager* fm = [NSFileManager defaultManager];
    if (!_inputIsSequence) {
        return [[fm attributesOfItemAtPath:[self inputPathString] error:NULL][NSFileSize] longLongValue];
    }
    std::vector<std::string> files = _sequenceFiles;
    if (files.empty()) {
        try {
            files = stackcore::list_image_sequence(_inputPath);
        } catch (const std::exception&) {
            return 0;
        }
    }
    long long total = 0;
    for (const std::string& f : files) {
        total += [[fm attributesOfItemAtPath:[NSString stringWithUTF8String:f.c_str()]
                                       error:NULL][NSFileSize] longLongValue];
    }
    return total;
}

- (void)selectQueueIndex:(NSInteger)index {
    if (index < 0 || index >= static_cast<NSInteger>([_items count])) return;
    if (index != _currentIndex) _bannerDismissed = NO;
    const BOOL sameInput = (index == _currentIndex);
    _currentIndex = index;
    [_queueTable selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(index)]
             byExtendingSelection:NO];

    QueueItem* item = _items[static_cast<NSUInteger>(index)];
    // フレーム範囲はそのファイルだけのもの。別のファイルへ引き継ぐと、
    // 短い動画では「範囲が空」で開けなくなる（解析済みならサイドカーから戻る）。
    if (!sameInput) {
        [_rangeStartField setStringValue:@""];
        [_rangeEndField setStringValue:@""];
    }
    _inputPath = std::string([[item path] UTF8String]);
    _inputIsSequence = [item isSequence];
    _sequenceFiles.clear();
    for (NSString* f in [item sequenceFiles]) _sequenceFiles.push_back([f UTF8String]);

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
    _mapReport.reset();
    _frameInfos.clear();
    _qualityOrder.clear();
    _manualPointsActive = NO;
    _manualPoints.clear();
    [[[self window] undoManager] removeAllActionsWithTarget:self];
    [self resetFinishingForNewStack];
    [_graph clearData];
    [_graph setCurrentFrame:-1];
    [_preview clearAlignmentPoints];
    [_bannerLabel setStringValue:@""];
    [_alignSummaryLabel setStringValue:@""];

    _referenceImage.reset();
    _rejectedFrames = 0;
    _byteOrderSuspect = NO;
    _looksLikeShallowDepth = NO;
    [_viewModeSegment setSelectedSegment:0];
    // 新しいファイルを選んだらフレームの並びを時系列に戻す（品質の順位がまだ無い）。
    _frameOrderByQuality = NO;
    [_frameOrderSegment setSelectedSegment:0];
    [_graphMode setSelectedSegment:0];
    [_graph setSortedByQuality:NO];
    // 新しいファイルを選んだら工程を最初に戻す。
    // 仕上げタブのまま別ファイルに切り替わると、結果の無いファイルに対して
    // ウェーブレットのつまみだけが見えている、という宙ぶらりんな画面になる。
    if (!sameInput) [self selectInspectorTab:0];

    // 1枚目を出しておく。何も映らないより、まず見えたほうがよい。
    // プレビュー用の入力は開いたままにし、スライダー操作で開き直さない。
    _previewSource.reset();
    [_openedInputSignature release];
    _openedInputSignature = nil;
    try {
        const stackcore::OpenOptions options = [self currentOpenOptions];
        _previewSource = std::shared_ptr<stackcore::VideoSource>(
            stackcore::open_video(_inputPath, options).release());
        _sourceFrames = _previewSource->frame_count();
        _sourceWidth = _previewSource->width();
        _sourceHeight = _previewSource->height();
        _sourceTotalFrames = (options.frame_start != 0 || options.frame_end != 0)
                                 ? stackcore::open_raw_video(_inputPath, options)->frame_count()
                                 : _sourceFrames;
        _byteOrderSuspect = _previewSource->byte_order_suspect() ? YES : NO;
        [_graph setDisplayOffset:_previewSource->original_index(0)];
        _openedInputSignature = [[self inputSignature] copy];

        // 16bitと名乗っているのに実測が12bit幅に収まっていないか。
        // そのままだと画像が暗いだけで、破綻はしないので気づきにくい。
        const stackcore::FrameStats stats = _previewSource->frame_stats(0);
        _looksLikeShallowDepth =
            (_previewSource->bit_depth() >= 15 && stats.max_value > 0 && stats.max_value < 4096) ? YES : NO;

        [_frameSlider setMinValue:0.0];
        [_frameSlider setMaxValue:std::max(0, _sourceFrames - 1)];
        [self refreshSelectionControl];
        [_frameSlider setDoubleValue:0.0];
        [self showSourceFrame:0];
        [_statusLabel setStringValue:
                          [NSString stringWithFormat:@"%@ — %@", [item displayName],
                                                     [NSString stringWithUTF8String:
                                                                   _previewSource->describe().c_str()]]];
        if ([item state] == QueueItemStateError) {
            // 開けるようになった（読み方を変えた等）なら、失敗の印を外す。
            [item setState:QueueItemStatePending];
            [item setMessage:@""];
            [self fillHeaderInfo:item];
            [self reloadQueueRow:index];
        }
    } catch (const std::exception& e) {
        _previewSource.reset();
        _sourceFrames = 0;
        [_preview clearImage];
        [_statusLabel setStringValue:[NSString stringWithUTF8String:e.what()]];
    }
    [self updateBanner];

    [self loadSidecarForCurrent];
    if (!_analysis) [self loadQualityCacheForCurrent];
    [self updateApOverlay];
    [self updateControlsEnabled];
    [self updateNamePreview];
    [self updateDrizzleEstimate];
    [self updateFrameInfoLabel];
}

- (void)removeSelectedFromQueue:(id)sender {
    (void)sender;
    const NSInteger row = [self contextQueueRow];
    if (row < 0 || row >= static_cast<NSInteger>([_items count]) || _running) return;
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
    _sequenceFiles.clear();
    _inputIsSequence = NO;
    _previewSource.reset();

    _qualityStage.reset();
    _globalStage.reset();
    [_qualitySignature release];
    _qualitySignature = nil;
    [_globalSignature release];
    _globalSignature = nil;
    _analysis.reset();
    [_analysisSignature release];
    _analysisSignature = nil;
    _mapReport.reset();
    _frameInfos.clear();
    _qualityOrder.clear();
    _referenceImage.reset();
    [self resetFinishingForNewStack];

    _manualPointsActive = NO;
    _manualPoints.clear();
    [[[self window] undoManager] removeAllActionsWithTarget:self];
    _sourceChannels = 1;
    _sourceFrames = 0;
    _sourceTotalFrames = 0;
    _sourceWidth = 0;
    _sourceHeight = 0;
    _rejectedFrames = 0;
    _byteOrderSuspect = NO;
    _looksLikeShallowDepth = NO;
    _bannerDismissed = NO;

    [_queueTable deselectAll:nil];
    [_graph clearData];
    [_graph setCurrentFrame:-1];
    [_preview clearImage];
    [_preview clearAlignmentPoints];
    [_apCountLabel setStringValue:@""];
    [_alignSummaryLabel setStringValue:@""];
    [_frameSlider setMinValue:0.0];
    [_frameSlider setMaxValue:0.0];
    [_frameSlider setDoubleValue:0.0];
    [_viewModeSegment setSelectedSegment:0];
    _frameOrderByQuality = NO;
    [_frameOrderSegment setSelectedSegment:0];
    [self selectInspectorTab:0];
    [_progress setDoubleValue:0.0];
    [_progress setHidden:YES];
    [_statusLabel
        setStringValue:LSLocalizedString(@"クリアしました — 新しい動画を追加してください")];

    [self updateBanner];
    [self updateNamePreview];
    [self updateDrizzleEstimate];
    [self updateFrameInfoLabel];
    [self updateControlsEnabled];
}

// ［クリア］: 入力キュー・処理結果に加えて、すべての設定と仕上げのつまみを初期値に戻す。
// 元動画・解析キャッシュ・書き出し済みファイル・プリセットには触れない。
- (void)clearWorkspace:(id)sender {
    (void)sender;
    if (_running) return;
    [_items removeAllObjects];
    [_queueTable reloadData];
    [self resetWorkspaceForNewProcessing];
    [self resetAllSettingsToDefaults];
    [_statusLabel
        setStringValue:LSLocalizedString(@"クリアしました（設定も初期値に戻しました）— 新しい動画を追加してください")];
}

- (void)clearForTesting {
    [self clearWorkspace:nil];
}

// ---- 右クリック -------------------------------------------------------------

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
    if (action == @selector(revealQueueItemInFinder:) || action == @selector(removeSelectedFromQueue:)) {
        const NSInteger row = [self contextQueueRow];
        const BOOL hasRow = row >= 0 && row < static_cast<NSInteger>([_items count]);
        if (action == @selector(revealQueueItemInFinder:)) return hasRow;
        return hasRow && !_running;
    }
    // メニューバーの項目。押しても何も起きない状態では灰色にする。
    if (action == @selector(analyze:)) return [_qualityButton isEnabled];
    if (action == @selector(align:)) return [_alignButton isEnabled];
    if (action == @selector(run:)) return [_stackButton isEnabled];
    if (action == @selector(save:)) return [_saveButton isEnabled];
    if (action == @selector(cancel:)) return _running;
    if (action == @selector(exportMultiplePercents:)) return [_multiExportButton isEnabled];
    if (action == @selector(toggleLeftPane:)) {
        [menuItem setState:[_leftPane isHidden] ? NSControlStateValueOff : NSControlStateValueOn];
        return YES;
    }
    if (action == @selector(toggleRightPane:)) {
        [menuItem setState:[_rightPane isHidden] ? NSControlStateValueOff : NSControlStateValueOn];
        return YES;
    }
    if (action == @selector(zoomIn:) || action == @selector(zoomOut:) ||
        action == @selector(zoomToFit:) || action == @selector(zoomActualPixels:)) {
        return [_preview hasImage];
    }
    if (action == @selector(openRecent:)) return !_running;
    return YES;
}

- (void)revealQueueItemInFinder:(id)sender {
    (void)sender;
    const NSInteger row = [self contextQueueRow];
    if (row < 0 || row >= static_cast<NSInteger>([_items count])) return;
    QueueItem* item = _items[static_cast<NSUInteger>(row)];
    NSString* path = [[item sequenceFiles] count] > 0 ? [[item sequenceFiles] firstObject] : [item path];
    [[NSWorkspace sharedWorkspace]
        activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:path] ]];
}

- (void)openDocument:(id)sender {
    (void)sender;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setAllowedFileTypes:LSInputFileTypes()];
    [panel setAllowsMultipleSelection:YES];
    [panel setCanChooseDirectories:YES];
    [panel setMessage:LSLocalizedString(@"動画、静止画（複数選ぶと1本の連番になります）、またはフォルダを選んでください")];
    if ([panel runModal] != NSModalResponseOK) return;
    NSMutableArray* paths = [NSMutableArray array];
    for (NSURL* url in [panel URLs]) [paths addObject:[url path]];
    [self addPathsToQueue:paths];
}

// ---- 最近使った項目とキューの保存 --------------------------------------------

- (NSArray*)recentPaths {
    NSArray* list = [[NSUserDefaults standardUserDefaults] arrayForKey:@"recentInputs"];
    return list ? list : @[];
}

- (void)noteRecentPath:(NSString*)path {
    // 自己検証の起動で利用者の履歴を書き換えない。
    if ([path length] == 0 || getenv("LUNASTACK_SNAPSHOT")) return;
    NSMutableArray* list = [[[self recentPaths] mutableCopy] autorelease];
    [list removeObject:path];
    [list insertObject:path atIndex:0];
    while ([list count] > kRecentLimit) [list removeLastObject];
    [[NSUserDefaults standardUserDefaults] setObject:list forKey:@"recentInputs"];
}

- (void)openRecent:(id)sender {
    NSString* path = [sender representedObject];
    if (![path isKindOfClass:[NSString class]]) return;
    if (![[NSFileManager defaultManager] fileExistsAtPath:path]) {
        [self showError:[NSString stringWithFormat:LSLocalizedString(@"見つかりません: %@"), path]
                  title:LSLocalizedString(@"開けませんでした")];
        return;
    }
    [self openFileAtPath:path];
}

- (void)clearRecent:(id)sender {
    (void)sender;
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:@"recentInputs"];
}

@end
