#import "MainWindowController_Private.h"

#include <algorithm>
#include <cmath>

#include "stackcore/debayer.hpp"
#include "stackcore/image_reader.hpp"
#include "stackcore/raw_reader.hpp"

// 一括処理（品質評価からスタックまで）、スタック済みの画像を仕上げ・出力で開く経路、
// モザイク合成（画像ファイルから／キューの素材をスタックしてから）。

namespace {

// 画像ファイルを1枚読む。Bayer のまま入っているもの（カメラのRAWなど）は色を付ける。
std::shared_ptr<stackcore::FrameBuffer> LoadImageFile(const std::string& path) {
    stackcore::ImageFileInfo info;
    auto raw = std::make_shared<stackcore::FrameBuffer>();
    stackcore::read_image_file(path, *raw, info);
    if (stackcore::is_bayer(info.color) && stackcore::is_supported_bayer(info.color)) {
        auto rgb = std::make_shared<stackcore::FrameBuffer>();
        stackcore::debayer(*raw, info.color, stackcore::DebayerMethod::MalvarHeCutler, *rgb);
        return rgb;
    }
    return raw;
}

// モザイクの結果に付ける仮のパス（書き出し名と保存先に使う）。
NSString* MosaicVirtualPath(NSString* firstPath, BOOL firstIsDirectory) {
    NSString* dir = firstIsDirectory ? firstPath : [firstPath stringByDeletingLastPathComponent];
    NSString* base = firstIsDirectory ? [firstPath lastPathComponent]
                                      : [[firstPath lastPathComponent] stringByDeletingPathExtension];
    return [dir stringByAppendingPathComponent:[base stringByAppendingString:@"_mosaic"]];
}

}  // namespace

@implementation MainWindowController (Mosaic)

// ---- 一括処理 ---------------------------------------------------------------
//
// いまの設定で、品質評価 → アライメント → スタックを続けて走らせる。
// すでに終わっていて、設定も変わっていない工程は飛ばす（結果は同じなので、やり直さない）。
// 工程そのものは通常のボタンと同じ経路を通る（結果の保存や画面の更新を別に持たない）。

- (void)runAll:(id)sender {
    (void)sender;
    if (_running || _inputPath.empty() || !_previewSource || [self currentItemIsImage]) return;
    _chainStages = YES;
    [self continueChainAfterStage:-1];
}

// finishedStage は終えたばかりの工程（JobStage の番号。始めるときは -1）。
- (void)continueChainAfterStage:(int)finishedStage {
    // 中断・終了の要求が工程の切れ目に届いていたら、次へ進まない。
    if (finishedStage >= 0 && (_cancelFlag->load() || _terminateAfterCancel || _closeAfterCancel)) {
        [_statusLabel setStringValue:LSLocalizedString(@"中断しました")];
        [self stopChain];
        return;
    }
    const JobStage next = [self alignmentUsable]
                              ? JobStage::Stack
                              : ([self qualityUsable] ? JobStage::Alignment : JobStage::Quality);
    if (static_cast<int>(next) <= finishedStage) {
        // 終えたばかりの工程が「まだ」と判定された（処理中に設定を変えた）。繰り返さずに止める。
        [_statusLabel setStringValue:LSLocalizedString(
                                         @"処理中に設定が変わったため、一括処理を止めました。もう一度実行してください")];
        [self stopChain];
        return;
    }
    [self beginJobStage:next];
    if (!_running) [self stopChain];  // 始められなかった
}

// 一括処理を途中で終える（中断・失敗・始められなかった）。
- (void)stopChain {
    _chainStages = NO;
    [self abortMosaicBatch];
    [self updateControlsEnabled];
    [self jobDidStop];
    if (_onRunFinished) _onRunFinished();
}

// ---- スタック済みの画像を仕上げ・出力で開く -----------------------------------

- (BOOL)currentItemIsImage {
    if (_currentIndex < 0 || _currentIndex >= static_cast<NSInteger>([_items count])) return NO;
    return [(QueueItem*)_items[static_cast<NSUInteger>(_currentIndex)] isImage];
}

// 画像の項目を選んだ。スタック結果と同じ扱いで仕上げ・出力へ送る
// （ウェーブレット・色・レベル補正・切り抜き・書き出しがそのまま使える）。
- (void)showImageItem:(QueueItem*)item {
    std::shared_ptr<stackcore::FrameBuffer> image = [item image];
    if (!image) {
        try {
            image = LoadImageFile(std::string([[item path] UTF8String]));
        } catch (const std::exception& e) {
            NSString* message = [NSString stringWithUTF8String:e.what()];
            [item setState:QueueItemStateError];
            [item setMessage:message];
            [self reloadQueueRow:_currentIndex];
            [_preview clearImage];
            [_statusLabel setStringValue:message];
            return;
        }
    }
    if ([item state] == QueueItemStateError) {
        [item setState:QueueItemStateImage];
        [item setMessage:@""];
        [self reloadQueueRow:_currentIndex];
    }
    _sourceChannels = image->channels();
    _sourceWidth = image->width();
    _sourceHeight = image->height();

    _stacked = image;
    [_stackedInfo release];
    _stackedInfo = [@{
        @"imageOnly" : @YES,
        @"mosaicPanels" : @([item panelCount]),
        @"drizzle" : @1.0,
        @"framesCombined" : @0,
    } copy];
    _stackedFrames.clear();
    _finishing = std::make_shared<stackcore::FinishingPipeline>();
    _finishing->set_input(_stacked, kWaveletLayers);
    [_viewModeSegment setSelectedSegment:2];
    [[_displayMenu itemAtIndex:1] setState:NSControlStateValueOff];
    [self apDisplayChanged:nil];
    [self selectInspectorTab:3];
    [self showFinishedOrStacked];
    [_statusLabel setStringValue:[NSString stringWithFormat:LSLocalizedString(@"%@ を仕上げ・出力で開きました — %d×%d"),
                                                            [item displayName], image->width(),
                                                            image->height()]];
}

// 画像を2枚以上まとめて渡されたときの開き方を尋ねる。
//   0 = 連番としてスタックする / 1 = それぞれ仕上げ・出力で開く / 2 = モザイク合成 / -1 = やめる
// 枚数が多い・RAWが混ざる・自己検証の起動では、尋ねずに連番にする。
- (NSInteger)askHowToOpenImages:(NSArray*)paths {
    if (getenv("LUNASTACK_SNAPSHOT") || _running || [paths count] > 40) return 0;
    for (NSString* path in paths) {
        if (stackcore::is_raw_image_path(std::string([path UTF8String]))) return 0;
    }
    NSAlert* alert = [[[NSAlert alloc] init] autorelease];
    [alert setMessageText:[NSString stringWithFormat:LSLocalizedString(@"画像 %lu 枚の開き方を選んでください"),
                                                     (unsigned long)[paths count]]];
    [alert setInformativeText:LSLocalizedString(
                                  @"撮影したコマなら連番としてスタックします。スタック済みの画像なら、仕上げ・出力で開くか、モザイクのパネルとしてつなぎます。")];
    [alert addButtonWithTitle:LSLocalizedString(@"連番としてスタック")];
    [alert addButtonWithTitle:LSLocalizedString(@"それぞれ仕上げで開く")];
    [alert addButtonWithTitle:LSLocalizedString(@"モザイク合成")];
    [alert addButtonWithTitle:LSLocalizedString(@"やめる")];
    const NSModalResponse response = [alert runModal];
    if (response == NSAlertFirstButtonReturn) return 0;
    if (response == NSAlertSecondButtonReturn) return 1;
    if (response == NSAlertThirdButtonReturn) return 2;
    return -1;
}

// ---- モザイク合成 ------------------------------------------------------------

- (void)mosaicMenuChosen:(id)sender {
    const NSInteger tag = [(NSMenuItem*)sender tag];
    if (tag == 1) [self mosaicFromFiles:nil];
    else if (tag == 2) [self mosaicFromQueue:nil];
}

// スタック済みの画像（パネル）を選んでつなぐ。
- (void)mosaicFromFiles:(id)sender {
    (void)sender;
    if (_running) return;
    NSMutableArray* types = [NSMutableArray array];
    for (NSString* ext in LSInputFileTypes()) {
        if (![@[ @"ser", @"avi", @"mov", @"mp4", @"m4v" ] containsObject:ext]) [types addObject:ext];
    }
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setAllowedFileTypes:types];
    [panel setAllowsMultipleSelection:YES];
    [panel setCanChooseDirectories:NO];
    [panel setMessage:LSLocalizedString(@"モザイクにするパネル（スタック済みの画像）を2枚以上選んでください。並び順は問いません")];
    if ([panel runModal] != NSModalResponseOK) return;
    NSMutableArray* paths = [NSMutableArray array];
    for (NSURL* url in [panel URLs]) [paths addObject:[url path]];
    [self startMosaicWithImagePaths:paths];
}

- (void)startMosaicWithImagePaths:(NSArray*)paths {
    if (_running) return;
    if ([paths count] < 2) {
        [self showError:LSLocalizedString(@"モザイク合成には、パネルの画像が2枚以上必要です")
                  title:LSLocalizedString(@"モザイク合成できません")];
        return;
    }
    NSMutableArray* names = [NSMutableArray array];
    for (NSString* path in paths) [names addObject:[path lastPathComponent]];
    [self runMosaicWithPanels:std::vector<std::shared_ptr<stackcore::FrameBuffer>>()
                        paths:paths
                        names:names
                  virtualPath:MosaicVirtualPath([paths firstObject], NO)];
}

// キューの素材（パネルごとの動画・静止画連番）を、いまの設定で順にスタックしてからつなぐ。
- (void)mosaicFromQueue:(id)sender {
    (void)sender;
    if (_running) return;
    NSMutableArray* sources = [NSMutableArray array];
    for (QueueItem* item in _items) {
        if (![item isImage] && [item state] != QueueItemStateError) [sources addObject:item];
    }
    if ([sources count] < 2) {
        [self showError:LSLocalizedString(
                            @"パネルごとの動画（または静止画連番）を、入力キューに2本以上入れてから実行してください")
                  title:LSLocalizedString(@"モザイク合成できません")];
        return;
    }
    if (!getenv("LUNASTACK_SNAPSHOT")) {
        NSAlert* alert = [[[NSAlert alloc] init] autorelease];
        [alert setMessageText:[NSString stringWithFormat:LSLocalizedString(@"キューの %lu 本をスタックしてモザイク合成します"),
                                                         (unsigned long)[sources count]]];
        [alert setInformativeText:LSLocalizedString(
                                      @"いまの設定（アライメント・スタック）で1本ずつ順にスタックし、終わったら自動でつないで、結果を仕上げ・出力に送ります。フレーム範囲と処理範囲は素材ごとのものを使います。")];
        [alert addButtonWithTitle:LSLocalizedString(@"開始")];
        [alert addButtonWithTitle:LSLocalizedString(@"やめる")];
        if ([alert runModal] != NSAlertFirstButtonReturn) return;
    }

    // どのパネルも同じ条件でスタックする。フレーム範囲は素材ごとのものなので持ち込まない。
    NSMutableDictionary* settings = [[[self settingsDictionary] mutableCopy] autorelease];
    [settings removeObjectForKey:@"rangeStart"];
    [settings removeObjectForKey:@"rangeEnd"];
    [self abortMosaicBatch];
    _mosaicBatchItems = [sources retain];
    _mosaicBatchSettings = [settings copy];
    _mosaicBatchPosition = 0;
    _mosaicBatchActive = YES;
    [self mosaicBatchStartNext];
}

- (void)mosaicBatchStartNext {
    if (!_mosaicBatchActive || _mosaicBatchPosition >= [_mosaicBatchItems count]) return;
    QueueItem* item = _mosaicBatchItems[_mosaicBatchPosition];
    const NSUInteger index = [_items indexOfObjectIdenticalTo:item];
    if (index == NSNotFound) {
        [_statusLabel setStringValue:LSLocalizedString(@"モザイク合成を止めました（素材がキューから外れました）")];
        [self stopChain];
        return;
    }
    [self selectQueueIndex:static_cast<NSInteger>(index)];
    // 開いた時点で、その素材の前回の解析に付いていた設定が画面に戻ることがある。
    // 始めたときの設定を当て直す（同じ設定なら、前回の解析結果をそのまま使える）。
    [self applySettingsDictionary:_mosaicBatchSettings includePostProcessing:NO];
    if (!_previewSource) {
        [self showError:[NSString stringWithFormat:LSLocalizedString(@"開けませんでした: %@"), [item displayName]]
                  title:LSLocalizedString(@"モザイク合成できません")];
        [self stopChain];
        return;
    }
    _chainStages = YES;
    [self continueChainAfterStage:-1];
}

// パネル1枚ぶんのスタックが終わった。次の素材へ進むか、全部そろったらつなぐ。
- (void)mosaicBatchCollect:(std::shared_ptr<stackcore::FrameBuffer>)panel {
    if (_cancelFlag->load() || _terminateAfterCancel || _closeAfterCancel) {
        [_statusLabel setStringValue:LSLocalizedString(@"中断しました")];
        [self stopChain];
        return;
    }
    _mosaicPanels.push_back(panel);
    ++_mosaicBatchPosition;
    if (_mosaicBatchPosition < [_mosaicBatchItems count]) {
        [self mosaicBatchStartNext];
        return;
    }
    NSMutableArray* names = [NSMutableArray array];
    for (QueueItem* item in _mosaicBatchItems) [names addObject:[item displayName]];
    QueueItem* first = [_mosaicBatchItems firstObject];
    NSString* virtualPath = MosaicVirtualPath([first path], [first isSequence]);
    const std::vector<std::shared_ptr<stackcore::FrameBuffer>> panels = _mosaicPanels;
    [self abortMosaicBatch];
    [self runMosaicWithPanels:panels paths:nil names:names virtualPath:virtualPath];
}

// 一括のモザイク処理の状態を片付ける（途中でも、終わったあとでも呼んでよい）。
- (void)abortMosaicBatch {
    _mosaicBatchActive = NO;
    [_mosaicBatchItems release];
    _mosaicBatchItems = nil;
    [_mosaicBatchSettings release];
    _mosaicBatchSettings = nil;
    _mosaicBatchPosition = 0;
    _mosaicPanels.clear();
}

// パネルをつなぐ。panels が空なら paths の画像を読んでから始める。
- (void)runMosaicWithPanels:(std::vector<std::shared_ptr<stackcore::FrameBuffer>>)panels
                      paths:(NSArray*)paths
                      names:(NSArray*)names
                virtualPath:(NSString*)virtualPath {
    if (_running) return;
    _running = YES;
    _cancelFlag->store(false);
    [self resetEta];
    [_progress setDoubleValue:0.0];
    [_progress setHidden:NO];
    [self updateControlsEnabled];
    [_statusLabel setStringValue:LSLocalizedString(@"モザイク合成を始めます…")];

    std::vector<std::string> files;
    for (NSString* path in paths) files.push_back(std::string([path UTF8String]));
    std::atomic<bool>* cancelFlag = _cancelFlag;
    MainWindowController* controller = self;

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        const stackcore::ProgressFn progress =
            [controller, cancelFlag](const char* stageName, int done, int total) -> bool {
            if (cancelFlag->load()) return false;
            const int step = total < 100 ? 1 : total / 100;
            if (done % step == 0 || done == total) {
                NSString* text = [NSString stringWithUTF8String:stageName];
                dispatch_async(dispatch_get_main_queue(), ^{
                    [controller reportStage:text done:done total:total];
                });
            }
            return true;
        };

        std::vector<std::shared_ptr<stackcore::FrameBuffer>> loaded = panels;
        std::shared_ptr<stackcore::FrameBuffer> result;
        std::vector<bool> placed;
        NSString* summary = @"";
        std::string error;
        bool cancelled = false;
        try {
            for (std::size_t i = 0; i < files.size(); ++i) {
                loaded.push_back(LoadImageFile(files[i]));
                if (!progress("モザイク: パネルの読み込み", static_cast<int>(i) + 1,
                              static_cast<int>(files.size()))) {
                    throw stackcore::Cancelled();
                }
            }
            std::vector<const stackcore::FrameBuffer*> pointers;
            for (const auto& p : loaded) pointers.push_back(p.get());
            stackcore::MosaicReport report;
            result = std::make_shared<stackcore::FrameBuffer>(
                stackcore::build_mosaic(pointers, stackcore::MosaicSettings(), progress, report));
            for (const stackcore::MosaicPlacement& p : report.panels) placed.push_back(p.placed);
            summary = report.used_rotation
                          ? [NSString stringWithFormat:LSLocalizedString(@"回転を補正（最大 %.2f°）・つなぎ目のずれ %.2f px"),
                                                       report.max_rotation_degrees, report.residual_rms]
                          : [NSString stringWithFormat:LSLocalizedString(@"つなぎ目のずれ %.2f px"),
                                                       report.residual_rms];
        } catch (const stackcore::Cancelled&) {
            cancelled = true;
        } catch (const std::exception& e) {
            error = e.what();
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            [controller finishMosaic:result
                              placed:placed
                             summary:summary
                               names:names
                         virtualPath:virtualPath
                               error:error
                           cancelled:cancelled];
        });
    });
}

- (void)finishMosaic:(std::shared_ptr<stackcore::FrameBuffer>)image
               placed:(std::vector<bool>)placed
              summary:(NSString*)summary
                names:(NSArray*)names
          virtualPath:(NSString*)virtualPath
                error:(const std::string&)error
            cancelled:(bool)cancelled {
    _running = NO;
    [_progress setDoubleValue:0.0];
    [_progress setHidden:YES];
    [self resetEta];

    if (cancelled || !error.empty() || !image) {
        [_statusLabel setStringValue:LSLocalizedString(cancelled ? @"中断しました" : @"モザイク合成できませんでした")];
        if (!cancelled) {
            [self showError:[NSString stringWithUTF8String:error.empty() ? "" : error.c_str()]
                      title:LSLocalizedString(@"モザイク合成できませんでした")];
        }
        [self updateControlsEnabled];
        [self jobDidStop];
        if (_onRunFinished) _onRunFinished();
        return;
    }

    int placedCount = 0;
    NSMutableArray* missing = [NSMutableArray array];
    for (std::size_t i = 0; i < placed.size(); ++i) {
        if (placed[i]) ++placedCount;
        else if (i < [names count]) [missing addObject:names[i]];
    }

    // 結果をキューに足して選ぶ（画像の項目として、仕上げ・出力で開く）。
    NSString* path = virtualPath;
    for (int suffix = 2;; ++suffix) {
        BOOL taken = NO;
        for (QueueItem* existing in _items) {
            if ([[existing path] isEqualToString:path]) taken = YES;
        }
        if (!taken) break;
        path = [virtualPath stringByAppendingFormat:@"_%d", suffix];
    }
    QueueItem* item = [QueueItem mosaicItemWithPath:path image:image panelCount:placedCount];
    [item setSubtitle:[NSString stringWithFormat:@"%d×%d · %@", image->width(), image->height(), summary]];
    [_items addObject:item];
    [_queueTable reloadData];
    [self selectQueueIndex:static_cast<NSInteger>([_items count]) - 1];

    [_statusLabel setStringValue:[NSString stringWithFormat:LSLocalizedString(@"モザイク合成が完了しました — %d 枚中 %d 枚を配置（%d×%d、%@）"),
                                                            static_cast<int>(placed.size()), placedCount,
                                                            image->width(), image->height(), summary]];
    [self notifyDone:[NSString stringWithFormat:LSLocalizedString(@"モザイク合成が完了しました（%d×%d）"),
                                                image->width(), image->height()]];
    if ([missing count] > 0) {
        [self showError:[NSString stringWithFormat:LSLocalizedString(
                                                       @"次のパネルは、ほかのパネルとの重なりが見つからなかったので入れていません:\n%@"),
                                                   [missing componentsJoinedByString:@"\n"]]
                  title:LSLocalizedString(@"配置できなかったパネルがあります")];
    }
    [self updateControlsEnabled];
    [self jobDidStop];
    if (_onRunFinished) _onRunFinished();
}

@end
