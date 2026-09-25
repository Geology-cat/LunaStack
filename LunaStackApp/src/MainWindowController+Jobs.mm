#import "MainWindowController_Private.h"

#include <algorithm>
#include <cmath>

#include "stackcore/map_pipeline.hpp"
#include "stackcore/video_source.hpp"
#include "stackcore/wavelet.hpp"

@implementation MainWindowController (Jobs)

// ---- 実行 -----------------------------------------------------------------

- (void)startRun {
    // GUI自己検証専用。通常のボタンは工程ごとに停止する。
    [self beginJobStage:JobStage::Full];
}

- (void)startAnalyzeOnly {
    [self analyze:nil];
}

- (void)startAlignmentOnly {
    [self align:nil];
}

- (void)startStackOnly {
    [self run:nil];
}

- (void)setFrameLimit:(int)limit {
    _frameLimit = limit;
}

- (void)analyze:(id)sender {
    (void)sender;
    [self beginJobStage:JobStage::Quality];
}

- (void)align:(id)sender {
    (void)sender;
    [self beginJobStage:JobStage::Alignment];
}

- (void)run:(id)sender {
    (void)sender;
    [self beginJobStage:JobStage::Stack];
}

- (void)cancel:(id)sender {
    (void)sender;
    _cancelFlag->store(true);
    [_statusLabel setStringValue:LSLocalizedString(@"中断しています…")];
}

- (void)beginJobStage:(JobStage)stage {
    if (_running || _inputPath.empty()) return;
    if (stage == JobStage::Alignment && ![self qualityUsable]) return;
    if (stage == JobStage::Stack && ![self alignmentUsable]) return;

    _running = YES;
    _cancelFlag->store(false);
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
    req.stage = stage;
    if (stage == JobStage::Alignment) req.quality = _qualityStage;
    if (stage == JobStage::Stack) {
        req.global = _globalStage;
        req.analysis = _analysis;
    }

    NSString* qualitySignature = [[self qualitySignature] copy];
    NSString* alignmentSignature = [[self analysisSignature] copy];
    if (stage == JobStage::Quality) {
        [_statusLabel setStringValue:LSLocalizedString(@"各フレームの品質を評価しています…")];
    } else if (stage == JobStage::Alignment) {
        [_statusLabel setStringValue:LSLocalizedString(@"位置合わせを実行しています…")];
    } else if (stage == JobStage::Stack) {
        [_statusLabel setStringValue:LSLocalizedString(@"解析結果を使ってスタックしています…")];
    } else {
        [_statusLabel setStringValue:LSLocalizedString(@"自己検証用の一括処理を開始します…")];
    }

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
            [controller finishJob:result
                 qualitySignature:qualitySignature
               alignmentSignature:alignmentSignature];
            [qualitySignature release];
            [alignmentSignature release];
        });
    });
}

- (void)finishJob:(const JobResult&)result
 qualitySignature:(NSString*)qualitySignature
alignmentSignature:(NSString*)alignmentSignature {
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

    if (result.stage == JobStage::Quality && result.quality) {
        _qualityStage = result.quality;
        [_qualitySignature release];
        _qualitySignature = [qualitySignature copy];

        // 品質を評価し直したら、それ以降の工程は新しい値に対して未実行である。
        _globalStage.reset();
        [_globalSignature release];
        _globalSignature = nil;
        _analysis.reset();
        [_analysisSignature release];
        _analysisSignature = nil;
        _referenceImage.reset();
        _stacked.reset();
        _displayed.reset();
        _wavelet.reset();
        [_preview clearAlignmentPoints];
        [self showFrames:result.frames];
        [_inspectorTab setSelectedSegment:1];
        [self updateInspectorVisibility];
        [_statusLabel
            setStringValue:[NSString stringWithFormat:
                                         LSLocalizedString(@"品質評価が完了しました — %dフレーム"),
                                                    static_cast<int>(result.frames.size())]];
        [self notifyDone:LSLocalizedString(@"品質評価が完了しました。次はアライメントです")];
        [self updateControlsEnabled];
        if (_onRunFinished) _onRunFinished();
        return;
    }

    if (result.stage == JobStage::Alignment && result.global) {
        _globalStage = result.global;
        [_globalSignature release];
        _globalSignature = [alignmentSignature copy];
        if (!result.analysis) {
            // 画像全体の位置合わせに切り替えたとき、以前の局所領域を残さない。
            _analysis.reset();
            [_analysisSignature release];
            _analysisSignature = nil;
            _referenceImage.reset();
            [_preview clearAlignmentPoints];
        }
        _stacked.reset();
        _displayed.reset();
        _wavelet.reset();
    }

    if (result.analysis) {
        _analysis = result.analysis;
        [_analysisSignature release];
        _analysisSignature = [alignmentSignature copy];
        if (result.stage == JobStage::Alignment || result.stage == JobStage::Full) {
            [self saveSidecarForCurrent];
        }
        [self rebuildReferenceImage];
    }

    if (result.stage == JobStage::Full && result.global) {
        _qualityStage = std::make_shared<stackcore::GlobalStageReport>(*result.global);
        _globalStage = result.global;
        [_qualitySignature release];
        _qualitySignature = [qualitySignature copy];
        [_globalSignature release];
        _globalSignature = [alignmentSignature copy];
    }
    [self showFrames:result.frames];

    if ((result.stage == JobStage::Stack || result.stage == JobStage::Full) && result.image) {
        _stacked = result.image;
        if (_currentIndex >= 0) {
            QueueItem* item = _items[static_cast<NSUInteger>(_currentIndex)];
            [item setState:QueueItemStateStacked];
            [item setMessage:@""];
            [_queueTable reloadData];
        }
        // 分解はここで1回だけ行う。以降スライダーを動かしても再構成しか走らない。
        _wavelet = std::make_shared<stackcore::WaveletSharpener>();
        _wavelet->analyze(*_stacked, kWaveletLayers);
        [_viewModeSegment setSelectedSegment:2];
        // 結果が出たら次の工程（仕上げ・書き出し）のタブへ進める。
        // 「スタックし終わったのに次に何をするのか分からない」を防ぐ。
        [_inspectorTab setSelectedSegment:3];
        [self updateInspectorVisibility];
        [self applyWavelet];
        [_statusLabel setStringValue:[NSString stringWithFormat:LSLocalizedString(@"完了 — %d×%d"),
                                                                _stacked->width(),
                                                                _stacked->height()]];
        [self notifyDone:[NSString stringWithFormat:LSLocalizedString(@"スタックが完了しました（%d×%d）"),
                                                    _stacked->width(), _stacked->height()]];
    } else if (result.stage == JobStage::Alignment) {
        [_inspectorTab setSelectedSegment:2];
        [self updateInspectorVisibility];
        [_statusLabel setStringValue:
                          [NSString stringWithFormat:
                                        LSLocalizedString(@"アライメントが完了しました — 位置合わせ領域 %d個 / %dフレーム"),
                                                     _analysis ? static_cast<int>(
                                                                     _analysis->points.size())
                                                               : 0,
                                                     static_cast<int>(result.frames.size())]];
        if (_referenceImage) {
            [_viewModeSegment setSelectedSegment:1];
            [_preview showFrameBuffer:*_referenceImage];
        }
        [self notifyDone:LSLocalizedString(@"アライメントが完了しました。次はスタックです")];
    }
    // **APオーバーレイの更新は _stacked を入れたあとに行う。**
    // オーバーレイの座標倍率はDrizzle倍率から決まるが、その判断に
    // 「スタック結果があるか」を使っている。順番を逆にすると、
    // 2倍で出した画像の上に等倍のAP枠が描かれ、左上の1/4に縮んで並ぶ。
    [self updateApOverlay];
    [self updateControlsEnabled];
    if (_onRunFinished) _onRunFinished();
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

@end
