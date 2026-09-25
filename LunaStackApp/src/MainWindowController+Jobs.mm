#import "MainWindowController_Private.h"

#include <algorithm>
#include <cmath>

@implementation MainWindowController (Jobs)

// ---- 自己検証用の入口 --------------------------------------------------------

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

// ---- ボタン -----------------------------------------------------------------

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

// 成功したら、以前の「失敗」の印を外す（B2）。
- (void)markCurrentItemSucceeded {
    if (_currentIndex < 0 || _currentIndex >= static_cast<NSInteger>([_items count])) return;
    QueueItem* item = _items[static_cast<NSUInteger>(_currentIndex)];
    if ([item state] == QueueItemStateError) {
        [item setState:QueueItemStatePending];
        [item setMessage:@""];
        [self fillHeaderInfo:item];
        [self reloadQueueRow:_currentIndex];
    }
}

// ---- ダーク・フラットのマスター作成 ---------------------------------------------

// 補正の素材が選ばれていてマスターが未作成なら、先に作ってから next を呼ぶ。
// マスターはフレームを全部読むので、品質評価と同じく中断できるようにする。
- (void)ensureCalibrationThen:(void (^)(void))next {
    if (!_calibrationDirty || (!_darkPath && !_flatPath)) {
        next();
        return;
    }
    _running = YES;
    _cancelFlag->store(false);
    [self resetEta];
    [_progress setDoubleValue:0.0];
    [_progress setHidden:NO];
    [self updateControlsEnabled];
    [_statusLabel setStringValue:LSLocalizedString(@"ダーク・フラットのマスターを作っています…")];

    stackcore::OpenOptions raw;
    raw.endian = [self currentOpenOptions].endian;
    raw.bit_depth_override = [self currentOpenOptions].bit_depth_override;
    // フラットの正規化はライト側の色配列に合わせる（手動指定があればそれ）。
    const stackcore::OpenOptions light = [self currentOpenOptions];
    stackcore::SerColorId pattern = stackcore::SerColorId::Mono;
    try {
        pattern = light.override_color ? light.color_override
                                       : stackcore::open_raw_video(_inputPath, light)->color_id();
    } catch (const std::exception&) {
    }
    const std::string dark = _darkPath ? std::string([_darkPath UTF8String]) : std::string();
    const std::string flat = _flatPath ? std::string([_flatPath UTF8String]) : std::string();
    NSString* identity = [NSString stringWithFormat:@"%@|%@", _darkPath ? _darkPath : @"",
                                                    _flatPath ? _flatPath : @""];
    std::atomic<bool>* cancelFlag = _cancelFlag;
    MainWindowController* controller = self;
    void (^done)(void) = [next copy];

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto cal = std::make_shared<stackcore::CalibrationFrames>();
        std::string error;
        bool cancelled = false;
        const auto progress_for = [controller, cancelFlag](const char* stage) {
            NSString* text = [NSString stringWithUTF8String:stage];
            return [controller, cancelFlag, text](int doneCount, int total) -> bool {
                if (cancelFlag->load()) return false;
                const int step = total < 100 ? 1 : total / 100;
                if (doneCount % step == 0 || doneCount == total) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        [controller reportStage:text done:doneCount total:total];
                    });
                }
                return true;
            };
        };
        try {
            if (!dark.empty()) {
                cal->dark = stackcore::build_master_frame(*stackcore::open_raw_video(dark, raw),
                                                          progress_for("マスターダーク作成"));
            }
            if (!flat.empty()) {
                const stackcore::FrameBuffer master = stackcore::build_master_frame(
                    *stackcore::open_raw_video(flat, raw), progress_for("マスターフラット作成"));
                cal->flat = stackcore::normalize_flat(master, pattern);
            }
            cal->identity = std::string([identity UTF8String]);
        } catch (const stackcore::Cancelled&) {
            cancelled = true;
        } catch (const std::exception& e) {
            error = e.what();
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            [controller finishCalibration:cal error:error cancelled:cancelled then:done];
        });
    });
}

- (void)finishCalibration:(std::shared_ptr<stackcore::CalibrationFrames>)cal
                    error:(const std::string&)error
                cancelled:(bool)cancelled
                     then:(void (^)(void))next {
    _running = NO;
    [_progress setHidden:YES];
    [self resetEta];
    if (cancelled || !error.empty()) {
        [_statusLabel setStringValue:LSLocalizedString(cancelled ? @"中断しました"
                                                                 : @"マスターを作れませんでした")];
        if (!error.empty()) {
            [self showError:[NSString stringWithUTF8String:error.c_str()]
                      title:LSLocalizedString(@"ダーク・フラットを使えません")];
        }
        [next release];
        [self updateControlsEnabled];
        [self jobDidStop];
        if (_onRunFinished) _onRunFinished();
        return;
    }
    _calibration = cal;
    _calibrationDirty = NO;
    // 補正を掛けた入力として開き直す（1枚目の見え方も変わる）。
    if (_currentIndex >= 0) [self selectQueueIndex:_currentIndex];
    if (!_previewSource) {
        // 寸法が合わない等でラッパーが開けなかった。理由はステータスに出ている。
        [self showError:[_statusLabel stringValue]
                  title:LSLocalizedString(@"ダーク・フラットを使えません")];
        [next release];
        [self updateControlsEnabled];
        if (_onRunFinished) _onRunFinished();
        return;
    }
    next();
    [next release];
}

// ---- 工程の実行 -------------------------------------------------------------

- (void)beginJobStage:(JobStage)stage {
    if (_running || _inputPath.empty()) return;
    if (stage == JobStage::Alignment && ![self qualityUsable]) return;
    if (stage == JobStage::Stack && ![self alignmentUsable]) return;
    MainWindowController* controller = self;
    [self ensureCalibrationThen:^{
        [controller startJobStage:stage];
    }];
}

// スタックしたときの条件。書き出し名とAP枠の倍率は、つまみではなくこちらに従う（B3・B4）。
- (NSDictionary*)stackInfoForCurrentSettings {
    const BOOL globalOnly = [_methodPopup indexOfSelectedItem] == 1;
    const int apSize = _analysis ? _analysis->ap_size : LSApSizeAt([_apSizePopup indexOfSelectedItem]);
    NSString* selection;
    if (globalOnly) {
        selection = [NSString stringWithFormat:@"%.0fpct", [_topSlider doubleValue]];
    } else if (_selectionUsesCount) {
        selection = [NSString stringWithFormat:@"%dframes", _apTopCountSetting];
    } else {
        selection = [NSString stringWithFormat:@"%.0fpct", _apTopPercentSetting];
    }
    return @{
        @"drizzle" : @(LSDrizzleScaleAt([_drizzleSegment selectedSegment])),
        @"pixfrac" : @([_pixfracSlider doubleValue]),
        @"apSize" : @(apSize),
        @"globalOnly" : @(globalOnly),
        @"selection" : selection,
        @"stackMode" : @([_stackModePopup indexOfSelectedItem]),
    };
}

- (void)startJobStage:(JobStage)stage {
    if (_running || _inputPath.empty()) return;
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
    NSDictionary* stackInfo = [[self stackInfoForCurrentSettings] retain];
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
            [controller, cancelFlag](const char* stageName, int done, int total) -> bool {
            if (cancelFlag->load()) return false;
            // 毎フレーム投げると描画で溢れるので、1%刻みに間引く。
            const int step = total < 100 ? 1 : total / 100;
            if (done % step == 0 || done == total) {
                NSString* text = [NSString stringWithUTF8String:stageName];
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
               alignmentSignature:alignmentSignature
                        stackInfo:stackInfo];
            [qualitySignature release];
            [alignmentSignature release];
            [stackInfo release];
        });
    });
}

- (void)finishJob:(const JobResult&)result
    qualitySignature:(NSString*)qualitySignature
    alignmentSignature:(NSString*)alignmentSignature
    stackInfo:(NSDictionary*)stackInfo {
    _running = NO;
    [_progress setDoubleValue:0.0];
    [_progress setHidden:YES];
    [self resetEta];

    if (result.cancelled) {
        [_statusLabel setStringValue:LSLocalizedString(@"中断しました")];
        [self updateControlsEnabled];
        [self jobDidStop];
        if (_onRunFinished) _onRunFinished();
        return;
    }
    if (!result.error.empty()) {
        [_statusLabel setStringValue:LSLocalizedString(@"失敗しました")];
        if (_currentIndex >= 0) {
            QueueItem* item = _items[static_cast<NSUInteger>(_currentIndex)];
            [item setState:QueueItemStateError];
            [item setMessage:[NSString stringWithUTF8String:result.error.c_str()]];
            [self reloadQueueRow:_currentIndex];
        }
        [self showError:[NSString stringWithUTF8String:result.error.c_str()]
                  title:LSLocalizedString(@"処理できませんでした")];
        [self updateControlsEnabled];
        [self jobDidStop];
        if (_onRunFinished) _onRunFinished();
        return;
    }
    [self markCurrentItemSucceeded];

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
        _mapReport.reset();
        _referenceImage.reset();
        [self resetFinishingForNewStack];
        [_preview clearAlignmentPoints];
        [_alignSummaryLabel setStringValue:@""];
        [self showFrames:result.frames];
        [self saveQualityCacheForCurrent];
        // 品質評価の後は、フレームのスライダーを品質順にする（良いものから順に見比べられる）。
        _frameOrderByQuality = YES;
        [_frameOrderSegment setSelectedSegment:1];
        [_graphMode setSelectedSegment:1];
        [_graph setSortedByQuality:YES];
        [self updateSliderRange];
        [_frameSlider setDoubleValue:0.0];
        [_viewModeSegment setSelectedSegment:0];
        [self showSourceFrame:[self currentFrameIndex]];
        [self selectInspectorTab:1];
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
        _mapReport = result.map_report;
        [self resetFinishingForNewStack];
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
        _mapReport = result.map_report;
        [_qualitySignature release];
        _qualitySignature = [qualitySignature copy];
        [_globalSignature release];
        _globalSignature = [alignmentSignature copy];
    }
    [self showFrames:result.frames];
    [_alignSummaryLabel setStringValue:[self analysisSummaryText]];

    if ((result.stage == JobStage::Stack || result.stage == JobStage::Full) && result.image) {
        [self resetFinishingForNewStack];
        _stacked = result.image;
        [_stackedInfo release];
        NSMutableDictionary* info = [[stackInfo mutableCopy] autorelease];
        info[@"framesCombined"] = @(result.frames_combined);
        _stackedInfo = [info copy];
        _stackedFrames = result.stacked_frames;
        if (_currentIndex >= 0) {
            QueueItem* item = _items[static_cast<NSUInteger>(_currentIndex)];
            [item setState:QueueItemStateStacked];
            [item setMessage:@""];
            [self reloadQueueRow:_currentIndex];
        }
        // 分解はここで1回だけ行う。以降スライダーを動かしても再構成しか走らない。
        _finishing = std::make_shared<stackcore::FinishingPipeline>();
        _finishing->set_input(_stacked, kWaveletLayers);
        [_viewModeSegment setSelectedSegment:2];
        // 結果が出たら次の工程（仕上げ・書き出し）のタブへ進める。
        // 「スタックし終わったのに次に何をするのか分からない」を防ぐ。
        [self selectInspectorTab:3];
        [self applyWavelet];
        [_statusLabel setStringValue:[NSString stringWithFormat:LSLocalizedString(@"完了 — %d×%d"),
                                                                _stacked->width(),
                                                                _stacked->height()]];
        [self notifyDone:[NSString stringWithFormat:LSLocalizedString(@"スタックが完了しました（%d×%d）"),
                                                    _stacked->width(), _stacked->height()]];
    } else if (result.stage == JobStage::Alignment) {
        [self selectInspectorTab:2];
        [_statusLabel setStringValue:
                          [NSString stringWithFormat:
                                        LSLocalizedString(@"アライメントが完了しました — 位置合わせ領域 %d個 / %dフレーム"),
                                                     _analysis ? static_cast<int>(
                                                                     _analysis->points.size())
                                                               : 0,
                                                     static_cast<int>(result.frames.size())]];
        if (_referenceImage) {
            [_viewModeSegment setSelectedSegment:1];
            [_preview showSharedFrame:_referenceImage];
        }
        [self notifyDone:LSLocalizedString(@"アライメントが完了しました。次はスタックです")];
    }
    // **APオーバーレイの更新は _stacked を入れたあとに行う。**
    // オーバーレイの座標倍率は「その画像を作ったときのDrizzle倍率」から決まる。
    // 順番を逆にすると、2倍で出した画像の上に等倍のAP枠が描かれ、左上の1/4に縮んで並ぶ。
    [self updateApOverlay];
    [self updateFrameInfoLabel];
    [self updateControlsEnabled];
    [self jobDidStop];
    if (_onRunFinished) _onRunFinished();
}

// ---- 進捗と残り時間 --------------------------------------------------------

- (void)resetEta {
    [_etaStage release];
    _etaStage = nil;
    _etaStart = 0.0;
}

- (void)reportStage:(NSString*)stage done:(int)done total:(int)total {
    if (!_running) return;  // 終わったあとに届いた古い通知は捨てる
    // 参照の反復精密化では、局所アライメントと窓合成を2回通る。エンジンは2回目の段に
    // 「(2回目)」を付けて知らせるので、それを見て全体の何パス目かを出す（U9）。
    const BOOL secondPass = [stage rangeOfString:@"(2回目)"].location != NSNotFound;
    const BOOL passStage = secondPass || [stage isEqualToString:@"局所アライメント"] ||
                           [stage isEqualToString:@"窓合成スタック"];
    const int passes = ([_refineCheck state] == NSControlStateValueOn &&
                        [_methodPopup indexOfSelectedItem] == 0)
                           ? 2
                           : 1;
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
    NSString* pass = (passStage && passes > 1)
                         ? [NSString stringWithFormat:LSLocalizedString(@"［パス %d/%d］"),
                                                      secondPass ? 2 : 1, passes]
                         : @"";
    [_statusLabel setStringValue:[NSString stringWithFormat:@"%@%@  %d / %d（%.0f%%）%@", pass,
                                                            stage, done, total, fraction * 100.0,
                                                            eta]];
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

- (void)showError:(NSString*)message title:(NSString*)title {
    if (getenv("LUNASTACK_SNAPSHOT")) {
        // 自己検証の起動では、ダイアログで止まらずログに残す。
        NSLog(@"%@: %@", title, message);
        return;
    }
    NSAlert* alert = [[[NSAlert alloc] init] autorelease];
    [alert setMessageText:LSLocalizedString(title)];
    [alert setInformativeText:message];
    [alert addButtonWithTitle:LSLocalizedString(@"OK")];
    [alert runModal];
}

@end
