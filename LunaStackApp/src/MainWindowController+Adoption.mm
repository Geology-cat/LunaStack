#import "MainWindowController_Private.h"

#include <algorithm>
#include <cmath>

// 採用率の目安。
//
// アライメント済みのフレームを2組に分けて別々にスタックし、両方に再現する細部の量を
// 採用率ごとに比べる（測り方は stackcore/adoption_diagnosis.hpp）。フレームを読み直すので、
// 工程と同じく進み具合を出し、中断できるようにする。
// 結果は解析結果と加算方式に対するものなので、どちらかが変わったら消す。

@implementation MainWindowController (Adoption)

- (NSString*)adoptionKeyNow {
    if (!_analysis) return nil;
    const stackcore::MapStackSettings s = [self currentSettings];
    return [NSString stringWithFormat:@"%p|%d|%d|%g", static_cast<const void*>(_analysis.get()),
                                      static_cast<int>(s.stack_mode), s.normalize_brightness ? 1 : 0,
                                      s.sigma_clip_threshold];
}

// いまの採用率（%）。枚数で指定しているときは、フレーム数に対する割合に直す。
- (double)adoptionCurrentPercent {
    if (!_selectionUsesCount) return _apTopPercentSetting;
    if (_adoptionFrames <= 0) return -1.0;
    return std::min(100.0, 100.0 * _apTopCountSetting / _adoptionFrames);
}

// 測った点のあいだを直線でつないだ、採用率 percent での点数。
- (double)adoptionScoreAt:(double)percent {
    if (_adoptionPercents.empty()) return 0.0;
    if (percent <= _adoptionPercents.front()) return _adoptionScores.front();
    for (std::size_t i = 1; i < _adoptionPercents.size(); ++i) {
        if (percent <= _adoptionPercents[i]) {
            const double t = (percent - _adoptionPercents[i - 1]) / (_adoptionPercents[i] - _adoptionPercents[i - 1]);
            return _adoptionScores[i - 1] + t * (_adoptionScores[i] - _adoptionScores[i - 1]);
        }
    }
    return _adoptionScores.back();
}

// 表示を、いまの採用率に合わせて作り直す（つまみを動かすたびに呼ぶ）。
- (void)refreshAdoptionDisplay {
    if (!_adoptionKey || _adoptionPercents.empty()) return;
    const double current = [self adoptionCurrentPercent];
    [_adoptionGraph setCurrentPercent:current];

    NSMutableString* text = [NSMutableString string];
    [text appendFormat:LSLocalizedString(@"目安は %.0f %% です。"), _adoptionSuggested];
    if (_adoptionBest != _adoptionSuggested) {
        [text appendFormat:LSLocalizedString(@"再現性が最も高いのは %.0f %% ですが、%.0f %% でもほぼ同じです。"), _adoptionBest,
                           _adoptionSuggested];
    }
    if (current >= 0.0) {
        const double best = *std::max_element(_adoptionScores.begin(), _adoptionScores.end());
        const double ratio = best > 0.0 ? 100.0 * [self adoptionScoreAt:current] / best : 0.0;
        [text appendFormat:@"\n%@",
                           [NSString stringWithFormat:LSLocalizedString(@"いまの採用率（%.0f %%、オレンジの線）の再現性は、最も高い値の %.0f %% です。"),
                                                      current, ratio]];
    }
    [text appendFormat:@"\n%@",
                       LSLocalizedString(@"フレームを2組に分けて別々にスタックし、両方に再現する細部の量を比べた目安です。見た目の鋭さの好みとは別です。")];
    [_adoptionLabel setStringValue:text];
    [_adoptionApplyButton setTitle:[NSString stringWithFormat:LSLocalizedString(@"%.0f %% にする"), _adoptionSuggested]];
    [_adoptionApplyButton setHidden:!_selectionUsesCount && std::fabs(_apTopPercentSetting - _adoptionSuggested) < 0.5];
}

- (void)showAdoptionDiagnosis:(const stackcore::AdoptionDiagnosis&)d key:(NSString*)key {
    [_adoptionKey release];
    _adoptionKey = [key copy];
    _adoptionPercents.clear();
    _adoptionScores.clear();
    _adoptionFrames = d.frames;
    [_adoptionLabel setHidden:NO];
    if (d.insufficient) {
        _adoptionSuggested = 0.0;
        _adoptionBest = 0.0;
        [_adoptionGraph clearData];
        [_adoptionGraph setHidden:YES];
        [_adoptionApplyButton setHidden:YES];
        [_adoptionLabel setStringValue:LSLocalizedString(
                                           @"測れませんでした（フレームが少ないか、位置合わせ領域に覆われた範囲が狭すぎます）")];
        return;
    }
    for (const stackcore::AdoptionPoint& p : d.points) {
        _adoptionPercents.push_back(p.percent);
        _adoptionScores.push_back(p.score);
    }
    _adoptionSuggested = d.suggested_percent;
    _adoptionBest = d.best_percent;
    [_adoptionGraph setPercents:_adoptionPercents.data()
                         scores:_adoptionScores.data()
                          count:static_cast<int>(_adoptionPercents.size())
                      suggested:_adoptionSuggested];
    [_adoptionGraph setHidden:NO];
    [self refreshAdoptionDisplay];
}

- (void)clearAdoptionDiagnosis {
    [_adoptionKey release];
    _adoptionKey = nil;
    _adoptionPercents.clear();
    _adoptionScores.clear();
    _adoptionSuggested = 0.0;
    _adoptionBest = 0.0;
    [_adoptionGraph clearData];
    [_adoptionGraph setHidden:YES];
    [_adoptionLabel setStringValue:@""];
    [_adoptionLabel setHidden:YES];
    [_adoptionApplyButton setHidden:YES];
}

- (void)diagnoseAdoption:(id)sender {
    (void)sender;
    if (_running || !_analysis || _inputPath.empty() || [_methodPopup indexOfSelectedItem] == 1) return;
    _running = YES;
    _cancelFlag->store(false);
    [self resetEta];
    [_progress setDoubleValue:0.0];
    [_progress setHidden:NO];
    [self updateControlsEnabled];
    [_statusLabel setStringValue:LSLocalizedString(@"採用率の目安を測っています…")];

    // 設定はUIスレッドで読み取ってから値渡しする。
    const std::string path = _inputPath;
    const stackcore::OpenOptions options = [self currentOpenOptions];
    stackcore::MapStackSettings settings = [self currentSettings];
    const bool lowMemory = [_lowMemoryCheck state] == NSControlStateValueOn;
    settings.low_memory = lowMemory;
    std::shared_ptr<stackcore::AnalysisData> analysis = _analysis;
    NSString* key = [[self adoptionKeyNow] retain];
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
        stackcore::AdoptionDiagnosis result;
        std::string error;
        bool cancelled = false;
        try {
            std::unique_ptr<stackcore::VideoSource> source = stackcore::open_video(path, options);
            if (lowMemory) source->set_low_memory(true);
            result = stackcore::diagnose_adoption(*source, settings, *analysis, progress);
        } catch (const stackcore::Cancelled&) {
            cancelled = true;
        } catch (const std::exception& e) {
            error = e.what();
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            [controller finishAdoption:result key:key error:error cancelled:cancelled];
            [key release];
        });
    });
}

- (void)finishAdoption:(const stackcore::AdoptionDiagnosis&)result
                   key:(NSString*)key
                 error:(const std::string&)error
             cancelled:(bool)cancelled {
    _running = NO;
    [_progress setDoubleValue:0.0];
    [_progress setHidden:YES];
    [self resetEta];
    if (cancelled) {
        [_statusLabel setStringValue:LSLocalizedString(@"中断しました")];
    } else if (!error.empty()) {
        [_statusLabel setStringValue:LSLocalizedString(@"採用率の目安を測れませんでした")];
        [self showError:[NSString stringWithUTF8String:error.c_str()]
                  title:LSLocalizedString(@"採用率の目安を測れませんでした")];
    } else if ([key isEqualToString:[self adoptionKeyNow]]) {
        [self showAdoptionDiagnosis:result key:key];
        [_statusLabel setStringValue:result.insufficient
                                         ? LSLocalizedString(@"採用率の目安を測れませんでした")
                                         : [NSString stringWithFormat:LSLocalizedString(@"採用率の目安は %.0f %% です"),
                                                                      result.suggested_percent]];
    }
    [self updateControlsEnabled];
    [self jobDidStop];
    if (_onRunFinished) _onRunFinished();
}

// 目安の採用率に合わせる（枚数で指定していたら、割合に切り替える）。
- (void)applyAdoptionSuggestion:(id)sender {
    (void)sender;
    if (_adoptionSuggested <= 0.0) return;
    _selectionUsesCount = NO;
    _apTopPercentSetting = _adoptionSuggested;
    [_selectionModeSegment setSelectedSegment:0];
    [self refreshSelectionControl];
    [self updateControlsEnabled];
}

@end
