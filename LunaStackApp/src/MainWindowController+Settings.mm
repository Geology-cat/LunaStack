#import "MainWindowController_Private.h"

#include <algorithm>
#include <cmath>

#include "stackcore/sidecar.hpp"

namespace {

// 数値欄を読む。空欄や不正な値なら fallback。範囲外は収める。
double FieldDouble(NSTextField* field, double fallback, double lo, double hi) {
    NSString* text = [[field stringValue] stringByTrimmingCharactersInSet:
                                              [NSCharacterSet whitespaceCharacterSet]];
    if ([text length] == 0) return fallback;
    NSScanner* scanner = [NSScanner scannerWithString:text];
    double v = 0.0;
    if (![scanner scanDouble:&v] || ![scanner isAtEnd] || !std::isfinite(v)) return fallback;
    return std::min(hi, std::max(lo, v));
}

int FieldInt(NSTextField* field, int fallback, int lo, int hi) {
    const double v = FieldDouble(field, fallback, lo, hi);
    return static_cast<int>(std::lround(v));
}

}  // namespace

@implementation MainWindowController (Settings)

// ---- 入力の読み方 -----------------------------------------------------------

// 入力の解釈（バイトオーダー・ビット深度・範囲・色・補正）。
// **解析結果に効く。** 変えたら品質評価からやり直しになる。
- (stackcore::OpenOptions)currentOpenOptions {
    stackcore::OpenOptions options;
    const stackcore::ByteOrder orders[] = {stackcore::ByteOrder::Auto, stackcore::ByteOrder::Little,
                                           stackcore::ByteOrder::Big};
    options.endian = orders[std::max<NSInteger>(0, [_endianPopup indexOfSelectedItem])];
    const int depths[] = {0, 12, 14};
    options.bit_depth_override = depths[std::max<NSInteger>(0, [_depthPopup indexOfSelectedItem])];

    // 範囲は1始まり・終了を含む表記で受け取り、エンジンの [開始, 終了) に直す。
    const int start = FieldInt(_rangeStartField, 1, 1, 100000000);
    const int end = FieldInt(_rangeEndField, 0, 0, 100000000);
    options.frame_start = start - 1;
    options.frame_end = end > 0 ? std::max(end, start) : 0;

    const NSInteger bayer = [_bayerPopup indexOfSelectedItem];
    if (bayer >= 1) {
        const stackcore::SerColorId ids[] = {stackcore::SerColorId::Mono,
                                             stackcore::SerColorId::BayerRGGB,
                                             stackcore::SerColorId::BayerGRBG,
                                             stackcore::SerColorId::BayerGBRG,
                                             stackcore::SerColorId::BayerBGGR};
        options.override_color = true;
        options.color_override = ids[std::min<NSInteger>(bayer - 1, 4)];
    }
    options.debayer = [_debayerPopup indexOfSelectedItem] == 1
                          ? stackcore::DebayerMethod::MalvarHeCutler
                          : stackcore::DebayerMethod::Bilinear;
    options.calibration = _calibration;
    options.sequence_files = _sequenceFiles;
    return options;
}

- (void)inputInterpretationChanged:(id)sender {
    (void)sender;
    if (_restoringSettings) return;
    // 数値欄は入力欄を離れただけでも action を送る。読み方が変わっていないのに
    // 開き直すと、スタック結果や仕上げを黙って捨ててしまう。
    if (_currentIndex >= 0 && _previewSource && _openedInputSignature &&
        [_openedInputSignature isEqualToString:[self inputSignature]]) {
        return;
    }
    _bannerDismissed = NO;
    // 読み方が変われば1枚目の見え方も変わる。開き直して確かめられるようにする。
    if (_currentIndex >= 0) [self selectQueueIndex:_currentIndex];
    [self updateControlsEnabled];
}

- (stackcore::MapStackSettings)currentSettings {
    stackcore::MapStackSettings settings;
    const stackcore::AlignMode modes[] = {stackcore::AlignMode::Auto, stackcore::AlignMode::Planet,
                                          stackcore::AlignMode::Lunar};
    settings.global.align.mode = modes[std::max<NSInteger>(0, [_modeSegment selectedSegment])];
    settings.global.limit = _frameLimit;
    settings.global.quality_metric =
        [_qualityMetricPopup indexOfSelectedItem] == 1
            ? stackcore::QualityMetric::FrequencyBandPowerRatio
            : stackcore::QualityMetric::GradientEnergy;
    settings.global.outlier_k = FieldDouble(_outlierKField, 6.0, 1.0, 50.0);
    settings.global.align.min_similarity = FieldDouble(_minSimilarityField, 0.5, -1.0, 1.0);
    settings.global.align.max_shift = FieldInt(_maxShiftField, 0, 0, 100000);
    settings.reference_top_percent = [_topSlider doubleValue];
    settings.ap_top_percent = _selectionUsesCount ? _apTopPercentSetting
                                                  : [_apTopSlider doubleValue];
    settings.ap_top_count = _selectionUsesCount
                                ? static_cast<int>([_apTopSlider doubleValue] + 0.5)
                                : 0;
    settings.normalize_brightness = [_normalizeCheck state] == NSControlStateValueOn;
    const stackcore::StackMode stackModes[] = {stackcore::StackMode::Mean,
                                               stackcore::StackMode::QualityWeighted,
                                               stackcore::StackMode::SigmaClip};
    settings.stack_mode = stackModes[std::max<NSInteger>(0, [_stackModePopup indexOfSelectedItem])];
    settings.sigma_clip_threshold = FieldDouble(_sigmaField, 2.0, 0.5, 10.0);
    settings.reference_passes = ([_refineCheck state] == NSControlStateValueOn) ? 2 : 1;

    settings.ap.ap_size = LSApSizeAt([_apSizePopup indexOfSelectedItem]);
    settings.ap.use_manual_points = _manualPointsActive ? true : false;
    settings.ap.manual_points = _manualPoints;
    settings.ap.gradient_ratio = FieldDouble(_apGradientField, 0.6, 0.0, 10.0);
    settings.ap.level_ratio = FieldDouble(_apLevelField, 0.15, 0.0, 1.0);
    settings.local.search_radius = static_cast<int>([_searchRadiusSlider doubleValue] + 0.5);
    settings.local.min_score = FieldDouble(_minScoreField, 0.5, -1.0, 1.0);

    settings.drizzle_scale = LSDrizzleScaleAt([_drizzleSegment selectedSegment]);
    settings.pixfrac = [_pixfracSlider doubleValue];
    settings.raw_cfa = [_rawCfaCheck state] == NSControlStateValueOn;
    return settings;
}

// ---- 解析結果を使い回せるかの判定 ---------------------------------------------

// 入力そのものと読み方。品質評価・アライメントの両方の指紋に入る。
- (NSString*)inputSignature {
    const stackcore::OpenOptions o = [self currentOpenOptions];
    NSMutableString* s = [NSMutableString string];
    [s appendFormat:@"path=%@;", [self inputPathString]];
    [s appendFormat:@"seq=%zu:", _sequenceFiles.size()];
    if (!_sequenceFiles.empty()) {
        [s appendFormat:@"%@|%@;", [NSString stringWithUTF8String:_sequenceFiles.front().c_str()],
                        [NSString stringWithUTF8String:_sequenceFiles.back().c_str()]];
    }
    [s appendFormat:@"limit=%d;endian=%ld;depth=%ld;", _frameLimit,
                    (long)[_endianPopup indexOfSelectedItem], (long)[_depthPopup indexOfSelectedItem]];
    [s appendFormat:@"range=%d-%d;bayer=%ld;debayer=%ld;raw=%d;", o.frame_start, o.frame_end,
                    (long)[_bayerPopup indexOfSelectedItem], (long)[_debayerPopup indexOfSelectedItem],
                    [_rawCfaCheck state] == NSControlStateValueOn ? 1 : 0];
    [s appendFormat:@"dark=%@;flat=%@;", _darkPath ? _darkPath : @"", _flatPath ? _flatPath : @""];
    return s;
}

// 品質評価結果を使い回してよいかを判断するための指紋。
// 位置合わせの設定は入れない。領域の大きさを変えても品質は再計算不要である。
- (NSString*)qualitySignature {
    return [NSString stringWithFormat:@"%@quality=%ld;", [self inputSignature],
                                      (long)[_qualityMetricPopup indexOfSelectedItem]];
}

// アライメント結果を使い回してよいかを判断するための指紋。
//
// **ここに入れる項目と入れない項目の区別が、このアプリの速さそのもの**である。
//   入れる  : 解析結果そのものが変わるもの（AP・参照・追跡）
//   入れない: 加算だけをやり直せば済むもの（AP別の選択率・Drizzle・低メモリ・加算方式）
- (NSString*)analysisSignature {
    const stackcore::MapStackSettings st = [self currentSettings];
    NSMutableString* s = [NSMutableString stringWithString:[self qualitySignature]];
    [s appendFormat:@"method=%ld;", (long)[_methodPopup indexOfSelectedItem]];
    [s appendFormat:@"mode=%ld;", (long)[_modeSegment selectedSegment]];
    [s appendFormat:@"apsize=%ld;", (long)[_apSizePopup indexOfSelectedItem]];
    [s appendFormat:@"radius=%.0f;", [_searchRadiusSlider doubleValue]];
    [s appendFormat:@"reftop=%.2f;", [_topSlider doubleValue]];
    [s appendFormat:@"passes=%d;", ([_refineCheck state] == NSControlStateValueOn) ? 2 : 1];
    [s appendFormat:@"outlier=%.3f;minsim=%.3f;maxshift=%d;", st.global.outlier_k,
                    st.global.align.min_similarity, st.global.align.max_shift];
    [s appendFormat:@"minscore=%.3f;apgrad=%.3f;aplevel=%.3f;norm=%d;", st.local.min_score,
                    st.ap.gradient_ratio, st.ap.level_ratio, st.normalize_brightness ? 1 : 0];
    [s appendFormat:@"manualOn=%d;manual=%zu:", _manualPointsActive ? 1 : 0,
                    _manualPoints.size()];
    for (std::size_t i = 0; i < _manualPoints.size(); ++i) {
        [s appendFormat:@"%d,%d;", _manualPoints[i].cx, _manualPoints[i].cy];
    }
    return s;
}

- (BOOL)analysisUsable {
    if (!_analysis || !_analysisSignature) return NO;
    return [_analysisSignature isEqualToString:[self analysisSignature]];
}

- (BOOL)qualityUsable {
    if (!_qualityStage || !_qualitySignature) return NO;
    return [_qualitySignature isEqualToString:[self qualitySignature]];
}

- (BOOL)globalUsable {
    if (!_globalStage || !_globalSignature) return NO;
    return [_globalSignature isEqualToString:[self analysisSignature]];
}

- (BOOL)alignmentUsable {
    return [_methodPopup indexOfSelectedItem] == 1 ? [self globalUsable]
                                                   : [self analysisUsable];
}

// 解析に影響する設定が変わった。
- (void)analysisSettingChanged:(id)sender {
    (void)sender;
    if (_restoringSettings) return;
    [self refreshCutLabel];
    [self updateControlsEnabled];
    [self updateNamePreview];
}

- (void)advancedFieldChanged:(id)sender {
    // 読めない値は既定値に戻して見せる（黙って別の値で動かさない）。
    NSTextField* field = (NSTextField*)sender;
    if ([field isKindOfClass:[NSTextField class]] && [[field stringValue] length] > 0) {
        NSScanner* scanner = [NSScanner scannerWithString:[field stringValue]];
        double v = 0.0;
        if (![scanner scanDouble:&v] || ![scanner isAtEnd]) {
            NSString* fallback = field == _outlierKField ? @"6.0"
                                 : field == _minSimilarityField ? @"0.5"
                                 : field == _minScoreField ? @"0.5"
                                 : field == _apGradientField ? @"0.6"
                                 : field == _apLevelField ? @"0.15"
                                 : field == _sigmaField ? @"2.0" : @"";
            [field setStringValue:fallback];
            NSBeep();
        }
    }
    [self analysisSettingChanged:sender];
}

- (void)topChanged:(id)sender {
    (void)sender;
    const BOOL wasUsable = [self alignmentUsable];
    [_topValue setStringValue:[NSString stringWithFormat:@"%.0f %%", [_topSlider doubleValue]]];
    [_graph setCutPercent:[_topSlider doubleValue]];
    [self updateControlsEnabled];
    if (wasUsable && ![self alignmentUsable] && !_restoringSettings) {
        [_statusLabel setStringValue:LSLocalizedString(
                                         @"参照フレームの割合を変えたので、アライメントのやり直しが必要です")];
    }
}

- (void)apTopChanged:(id)sender {
    (void)sender;
    if (_selectionUsesCount) {
        _apTopCountSetting = static_cast<int>([_apTopSlider doubleValue] + 0.5);
        [_apTopValue setStringValue:
                         [NSString stringWithFormat:LSLocalizedString(@"%d 枚"),
                                                    _apTopCountSetting]];
    } else {
        _apTopPercentSetting = [_apTopSlider doubleValue];
        [_apTopValue setStringValue:[NSString stringWithFormat:@"%.0f %%", _apTopPercentSetting]];
    }
    [self updateControlsEnabled];
}

- (void)selectionModeChanged:(id)sender {
    (void)sender;
    if (_selectionUsesCount) {
        _apTopCountSetting = static_cast<int>([_apTopSlider doubleValue] + 0.5);
    } else {
        _apTopPercentSetting = [_apTopSlider doubleValue];
    }
    _selectionUsesCount = [_selectionModeSegment selectedSegment] == 1;
    [self refreshSelectionControl];
    [self updateControlsEnabled];
}

- (void)refreshSelectionControl {
    if (_selectionUsesCount) {
        const int available = _frameLimit > 0 ? std::min(_sourceFrames, _frameLimit) : _sourceFrames;
        // ファイルを開く前に枚数プリセットを選んでも値を1へ潰さない。
        const int maximum = available > 0 ? available : std::max(1, _apTopCountSetting);
        _apTopCountSetting = std::max(1, std::min(_apTopCountSetting, maximum));
        [_apTopCaption setStringValue:
                           LSLocalizedString(@"位置合わせ領域ごとに採用するフレーム（枚数）")];
        [_apTopSlider setMinValue:1.0];
        [_apTopSlider setMaxValue:maximum];
        [_apTopSlider setDoubleValue:_apTopCountSetting];
    } else {
        [_apTopCaption setStringValue:
                           LSLocalizedString(@"位置合わせ領域ごとに採用するフレーム（%）")];
        [_apTopSlider setMinValue:1.0];
        [_apTopSlider setMaxValue:100.0];
        [_apTopSlider setDoubleValue:_apTopPercentSetting];
    }
    [self apTopChanged:nil];
}

- (void)searchRadiusChanged:(id)sender {
    (void)sender;
    [_searchRadiusValue setStringValue:[NSString stringWithFormat:@"±%.0f",
                                                                 [_searchRadiusSlider doubleValue]]];
    [self updateControlsEnabled];
}

- (void)drizzleChanged:(id)sender {
    (void)sender;
    [_pixfracValue setStringValue:[NSString stringWithFormat:@"%.2f", [_pixfracSlider doubleValue]]];
    [self updateDrizzleEstimate];
    [self updateControlsEnabled];
}

- (void)updateDrizzleEstimate {
    const double scale = LSDrizzleScaleAt([_drizzleSegment selectedSegment]);
    if (scale <= 1.0 || _sourceWidth <= 0) {
        [_drizzleEstimate setStringValue:@""];
        return;
    }
    const int w = static_cast<int>(_sourceWidth * scale);
    const int h = static_cast<int>(_sourceHeight * scale);
    // 出力＋重みで float 2面ぶんを持つ。
    const double mb = static_cast<double>(w) * h * _sourceChannels * 4 * 2 / (1024.0 * 1024.0);
    [_drizzleEstimate
        setStringValue:[NSString stringWithFormat:LSLocalizedString(@"出力 %d×%d ／ 作業メモリ約 %.0f MB"),
                                                  w, h, mb]];
}

// グラフのカットラインが何を決めているかを示す。
// 画像全体の位置合わせのみでは、同じ割合がスタックの採用率にもなる。
- (void)refreshCutLabel {
    const BOOL globalOnly = [_methodPopup indexOfSelectedItem] == 1;
    [_graph setCutLabel:LSLocalizedString(globalOnly ? @"参照・スタック" : @"参照")];
    [_graphHint setStringValue:LSLocalizedString(
                                   globalOnly
                                       ? @"オレンジの線は参照とスタックに使う上位の割合です。線をドラッグで変更、ほかの場所をクリックでそのフレームを表示します"
                                       : @"オレンジの線は参照画像に使う上位の割合です。線をドラッグで変更、ほかの場所をクリックでそのフレームを表示します")];
}

- (void)qualityGraphView:(QualityGraphView*)view didChangeCutPercent:(double)percent {
    (void)view;
    double p = percent;
    if (p < [_topSlider minValue]) p = [_topSlider minValue];
    if (p > [_topSlider maxValue]) p = [_topSlider maxValue];
    [_topSlider setDoubleValue:p];
    [self topChanged:nil];
}

- (void)updateControlsEnabled {
    const BOOL hasFile = !_inputPath.empty() && _previewSource != nullptr;
    const BOOL globalOnly = [_methodPopup indexOfSelectedItem] == 1;
    const BOOL qualityOk = [self qualityUsable];
    const BOOL alignmentOk = [self alignmentUsable];

    // **実行中は選択を変えさせない。**
    // 変えると _inputPath が差し替わり、走り終わった仕事の解析結果が
    // 「今選んでいる別のファイル」のサイドカーとして書き出されてしまう。
    [_queueTable setEnabled:!_running];

    [_qualityButton setEnabled:hasFile && !_running];
    [_alignButton setEnabled:hasFile && qualityOk && !_running];
    [_stackButton setEnabled:hasFile && alignmentOk && !_running];
    [_clearButton setEnabled:!_running];
    [_cancelButton setEnabled:_running];
    [_saveButton setEnabled:(_stacked != nullptr) && !_running];
    [_exportButton setEnabled:(_stacked != nullptr) && !_running];
    [_multiExportButton setEnabled:hasFile && alignmentOk && _stacked != nullptr && !_running];

    [_apSizePopup setEnabled:!globalOnly];
    [_apTopSlider setEnabled:!globalOnly];
    [_selectionModeSegment setEnabled:!globalOnly];
    [_qualityMetricPopup setEnabled:!_running];
    [_stackModePopup setEnabled:!globalOnly && !_running];
    [_sigmaField setEnabled:!globalOnly && [_stackModePopup indexOfSelectedItem] == 2];
    [_refineCheck setEnabled:!globalOnly];
    [_searchRadiusSlider setEnabled:!globalOnly];
    [_minScoreField setEnabled:!globalOnly];
    [_apGradientField setEnabled:!globalOnly];
    [_apLevelField setEnabled:!globalOnly];
    [_apEditCheck setEnabled:!globalOnly];
    [_methodPopup setEnabled:!_running];

    // チャンネル合わせと色はカラーの結果にだけ意味がある。
    const BOOL color = _stacked ? _stacked->channels() == 3 : _sourceChannels == 3;
    for (int i = 0; i < 4; ++i) [_channelFields[i] setEnabled:color];
    for (int c = 0; c < 3; ++c) [_gainSliders[c] setEnabled:color];
    [_saturationSlider setEnabled:color];

    // 各工程の完了状態を、押す前に分かるようにする。
    [_qualityButton setTitle:LSLocalizedString(qualityOk ? @"品質を再評価" : @"品質評価")];
    [_alignButton setTitle:LSLocalizedString(alignmentOk ? @"再アライメント"
                                                    : @"アライメント")];
    [_stackButton setTitle:LSLocalizedString(_stacked ? @"再スタック" : @"スタック")];
    if (_currentIndex >= 0 && _currentIndex < static_cast<NSInteger>([_items count]) && !_running) {
        QueueItem* item = _items[static_cast<NSUInteger>(_currentIndex)];
        if ([item state] != QueueItemStateError && [item state] != QueueItemStateStacked) {
            const QueueItemState next = alignmentOk ? QueueItemStateAnalyzed
                                                    : (qualityOk ? QueueItemStateQualityEvaluated
                                                                 : QueueItemStatePending);
            if ([item state] != next) {
                [item setState:next];
                [self reloadQueueRow:_currentIndex];
            }
        }
    }
    [self updateNamePreview];
}

// ---- 設定の辞書表現・保存・復元 ------------------------------------------------

// プリセットと起動間の保存の両方で使う、設定一式の辞書表現。
- (NSDictionary*)settingsDictionary {
    NSMutableArray* sharpen = [NSMutableArray array];
    NSMutableArray* denoise = [NSMutableArray array];
    for (int j = 0; j < kWaveletLayers; ++j) {
        [sharpen addObject:@([_sharpenSliders[j] doubleValue])];
        [denoise addObject:@([_denoiseSliders[j] doubleValue])];
    }
    NSMutableArray* channels = [NSMutableArray array];
    for (int i = 0; i < 4; ++i) [channels addObject:[_channelFields[i] stringValue]];
    NSMutableArray* gains = [NSMutableArray array];
    for (int c = 0; c < 3; ++c) [gains addObject:@([_gainSliders[c] doubleValue])];
    return @{
        @"method" : @([_methodPopup indexOfSelectedItem]),
        @"mode" : @([_modeSegment selectedSegment]),
        @"apSize" : @([_apSizePopup indexOfSelectedItem]),
        @"searchRadius" : @([_searchRadiusSlider doubleValue]),
        @"qualityMetric" : @([_qualityMetricPopup indexOfSelectedItem]),
        @"referenceTopPercent" : @([_topSlider doubleValue]),
        @"refine" : @([_refineCheck state] == NSControlStateValueOn),
        @"selectionMode" : @(_selectionUsesCount ? 1 : 0),
        @"apTopPercent" : @(_apTopPercentSetting),
        @"apTopCount" : @(_apTopCountSetting),
        @"normalizeBrightness" : @([_normalizeCheck state] == NSControlStateValueOn),
        @"stackMode" : @([_stackModePopup indexOfSelectedItem]),
        @"sigma" : [_sigmaField stringValue],
        @"lowMemory" : @([_lowMemoryCheck state] == NSControlStateValueOn),
        @"rawCfa" : @([_rawCfaCheck state] == NSControlStateValueOn),
        @"drizzle" : @([_drizzleSegment selectedSegment]),
        @"pixfrac" : @([_pixfracSlider doubleValue]),
        @"sharpen" : sharpen,
        @"denoise" : denoise,
        @"linked" : @([_linkedCheck state] == NSControlStateValueOn),
        @"linkedAmount" : @([_linkedSlider doubleValue]),
        @"dering" : @([_deringSlider doubleValue]),
        @"channelOffsets" : channels,
        @"gains" : gains,
        @"saturation" : @([_saturationSlider doubleValue]),
        @"levels" : [self levelsArray],
        @"rotation" : @(_rotationTurns),
        @"flipH" : @([_flipHCheck state] == NSControlStateValueOn),
        @"flipV" : @([_flipVCheck state] == NSControlStateValueOn),
        @"format" : @([_formatPopup indexOfSelectedItem]),
        @"nameStyle" : @([_nameStylePopup indexOfSelectedItem]),
        @"metadata" : @([_metadataCheck state] == NSControlStateValueOn),
        @"object" : [_objectField stringValue],
        @"multiPercents" : [_multiPercentField stringValue],
        @"endian" : @([_endianPopup indexOfSelectedItem]),
        @"bitDepth" : @([_depthPopup indexOfSelectedItem]),
        @"bayer" : @([_bayerPopup indexOfSelectedItem]),
        @"debayer" : @([_debayerPopup indexOfSelectedItem]),
        @"rangeStart" : [_rangeStartField stringValue],
        @"rangeEnd" : [_rangeEndField stringValue],
        @"outlierK" : [_outlierKField stringValue],
        @"minSimilarity" : [_minSimilarityField stringValue],
        @"maxShift" : [_maxShiftField stringValue],
        @"minScore" : [_minScoreField stringValue],
        @"apGradient" : [_apGradientField stringValue],
        @"apLevel" : [_apLevelField stringValue],
    };
}

- (void)applySettingsDictionary:(NSDictionary*)d {
    [self applySettingsDictionary:d includePostProcessing:YES];
}

static void SelectIndex(NSPopUpButton* popup, id value) {
    if (!value) return;
    const NSInteger i = [value integerValue];
    if (i >= 0 && i < [popup numberOfItems]) [popup selectItemAtIndex:i];
}

static void SelectSegment(NSSegmentedControl* control, id value) {
    if (!value) return;
    const NSInteger i = [value integerValue];
    if (i >= 0 && i < [control segmentCount]) [control setSelectedSegment:i];
}

static void SetCheck(NSButton* check, id value) {
    if (value) [check setState:[value boolValue] ? NSControlStateValueOn : NSControlStateValueOff];
}

static void SetText(NSTextField* field, id value) {
    if ([value isKindOfClass:[NSString class]]) [field setStringValue:value];
}

// includePostProcessing が NO のとき、仕上げのつまみは触らない。
//
// サイドカーの読み込みで使う。解析結果に付いてきた設定を戻すのは
// 「画面と中身を一致させる」ためであって、**仕上げは解析と無関係**である。
// 触っていた仕上げが、ファイルを開き直しただけで勝手に動くのは
// 「後処理は非破壊」（UI設計書 §1.5）に反する。
- (void)applySettingsDictionary:(NSDictionary*)d includePostProcessing:(BOOL)includePost {
    const BOOL wasRestoring = _restoringSettings;
    _restoringSettings = YES;
    SelectIndex(_methodPopup, d[@"method"]);
    SelectSegment(_modeSegment, d[@"mode"]);
    SelectIndex(_apSizePopup, d[@"apSize"]);
    if (d[@"searchRadius"]) [_searchRadiusSlider setDoubleValue:[d[@"searchRadius"] doubleValue]];
    SelectIndex(_qualityMetricPopup, d[@"qualityMetric"]);
    if (d[@"referenceTopPercent"]) [_topSlider setDoubleValue:[d[@"referenceTopPercent"] doubleValue]];
    SetCheck(_refineCheck, d[@"refine"]);
    if (d[@"apTopPercent"]) _apTopPercentSetting = [d[@"apTopPercent"] doubleValue];
    if (d[@"apTopCount"]) _apTopCountSetting = [d[@"apTopCount"] intValue];
    if (d[@"selectionMode"]) _selectionUsesCount = [d[@"selectionMode"] integerValue] == 1;
    [_selectionModeSegment setSelectedSegment:_selectionUsesCount ? 1 : 0];
    SetCheck(_normalizeCheck, d[@"normalizeBrightness"]);
    SelectIndex(_stackModePopup, d[@"stackMode"]);
    SetText(_sigmaField, d[@"sigma"]);
    SetCheck(_lowMemoryCheck, d[@"lowMemory"]);
    SetCheck(_rawCfaCheck, d[@"rawCfa"]);
    SelectSegment(_drizzleSegment, d[@"drizzle"]);
    if (d[@"pixfrac"]) [_pixfracSlider setDoubleValue:[d[@"pixfrac"] doubleValue]];
    SelectIndex(_formatPopup, d[@"format"]);
    SelectIndex(_nameStylePopup, d[@"nameStyle"]);
    SetCheck(_metadataCheck, d[@"metadata"]);
    SetText(_objectField, d[@"object"]);
    SetText(_multiPercentField, d[@"multiPercents"]);
    SelectIndex(_endianPopup, d[@"endian"]);
    SelectIndex(_depthPopup, d[@"bitDepth"]);
    SelectIndex(_bayerPopup, d[@"bayer"]);
    SelectIndex(_debayerPopup, d[@"debayer"]);
    SetText(_rangeStartField, d[@"rangeStart"]);
    SetText(_rangeEndField, d[@"rangeEnd"]);
    SetText(_outlierKField, d[@"outlierK"]);
    SetText(_minSimilarityField, d[@"minSimilarity"]);
    SetText(_maxShiftField, d[@"maxShift"]);
    SetText(_minScoreField, d[@"minScore"]);
    SetText(_apGradientField, d[@"apGradient"]);
    SetText(_apLevelField, d[@"apLevel"]);

    if (includePost) {
        NSArray* sharpen = d[@"sharpen"];
        NSArray* denoise = d[@"denoise"];
        for (int j = 0; j < kWaveletLayers; ++j) {
            if ([sharpen isKindOfClass:[NSArray class]] &&
                static_cast<NSUInteger>(j) < [sharpen count]) {
                [_sharpenSliders[j] setDoubleValue:[sharpen[static_cast<NSUInteger>(j)] doubleValue]];
            }
            if ([denoise isKindOfClass:[NSArray class]] &&
                static_cast<NSUInteger>(j) < [denoise count]) {
                [_denoiseSliders[j] setDoubleValue:[denoise[static_cast<NSUInteger>(j)] doubleValue]];
            }
        }
        SetCheck(_linkedCheck, d[@"linked"]);
        if (d[@"linkedAmount"]) [_linkedSlider setDoubleValue:[d[@"linkedAmount"] doubleValue]];
        if (d[@"dering"]) [_deringSlider setDoubleValue:[d[@"dering"] doubleValue]];
        NSArray* channels = d[@"channelOffsets"];
        for (int i = 0; i < 4; ++i) {
            if ([channels isKindOfClass:[NSArray class]] && static_cast<NSUInteger>(i) < [channels count]) {
                SetText(_channelFields[i], channels[static_cast<NSUInteger>(i)]);
            }
        }
        NSArray* gains = d[@"gains"];
        for (int c = 0; c < 3; ++c) {
            if ([gains isKindOfClass:[NSArray class]] && static_cast<NSUInteger>(c) < [gains count]) {
                [_gainSliders[c] setDoubleValue:[gains[static_cast<NSUInteger>(c)] doubleValue]];
            }
        }
        if (d[@"saturation"]) [_saturationSlider setDoubleValue:[d[@"saturation"] doubleValue]];
        [self setLevelsFromDictionary:d];
        if (d[@"rotation"]) _rotationTurns = (([d[@"rotation"] intValue] % 4) + 4) % 4;
        SetCheck(_flipHCheck, d[@"flipH"]);
        SetCheck(_flipVCheck, d[@"flipV"]);
    }

    [self topChanged:nil];
    [self refreshSelectionControl];
    [self searchRadiusChanged:nil];
    [self drizzleChanged:nil];
    [self refreshCutLabel];
    _restoringSettings = wasRestoring;
    [self updateFinishingValueLabels];
    if (includePost) [self requestFinishingRender];
    [self updateControlsEnabled];
}

// すべての設定を起動直後の状態に戻す（［クリア］）。
- (void)resetAllSettingsToDefaults {
    if (_defaultSettings) [self applySettingsDictionary:_defaultSettings includePostProcessing:YES];
    // 辞書に入っていない状態も初期に戻す。
    _rotationTurns = 0;
    _cropRect = NSZeroRect;
    [_cropModeCheck setState:NSControlStateValueOff];
    [_levelsChannelPopup selectItemAtIndex:0];
    [_waveletPreviewCheck setState:NSControlStateValueOn];
    [_waveletOnlyPreviewCheck setState:NSControlStateValueOn];
    [_linkedCheck setState:NSControlStateValueOff];
    [_lowMemoryCheck setState:NSControlStateValueOff];
    [_darkPath release];
    _darkPath = nil;
    [_flatPath release];
    _flatPath = nil;
    _calibration.reset();
    _calibrationDirty = NO;
    [_darkLabel setStringValue:LSLocalizedString(@"なし")];
    [_flatLabel setStringValue:LSLocalizedString(@"なし")];
    [_presetPopup selectItemAtIndex:0];
    [_apEditCheck setState:NSControlStateValueOff];
    [[_displayMenu itemAtIndex:1] setState:NSControlStateValueOn];
    [[_displayMenu itemAtIndex:2] setState:NSControlStateValueOff];
    [[_displayMenu itemAtIndex:3] setState:NSControlStateValueOn];
    [_preview setDisplayStretch:YES];
    [self apDisplayChanged:nil];
    [_zoomControl setSelectedSegment:0];
    [_preview setZoom:0.0];
    [_graphMode setSelectedSegment:0];
    [_frameOrderSegment setSelectedSegment:0];
    [self updateFinishingValueLabels];
    [self refreshCutLabel];
    [self updateControlsEnabled];
}

// ---- ダーク・フラット ---------------------------------------------------------

- (NSString*)chooseCalibrationSourceWithMessage:(NSString*)message {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setAllowedFileTypes:LSInputFileTypes()];
    [panel setCanChooseDirectories:YES];
    [panel setAllowsMultipleSelection:NO];
    [panel setMessage:message];
    if ([panel runModal] != NSModalResponseOK) return nil;
    return [[[panel URLs] firstObject] path];
}

- (void)invalidateCalibration {
    _calibration.reset();
    _calibrationDirty = (_darkPath != nil || _flatPath != nil);
    [_darkLabel setStringValue:_darkPath ? [_darkPath lastPathComponent] : LSLocalizedString(@"なし")];
    [_flatLabel setStringValue:_flatPath ? [_flatPath lastPathComponent] : LSLocalizedString(@"なし")];
    // 補正が変われば品質評価からやり直し（指紋に入っているので自動的に無効になる）。
    if (_currentIndex >= 0) [self selectQueueIndex:_currentIndex];
    [self updateControlsEnabled];
}

- (void)chooseDark:(id)sender {
    (void)sender;
    NSString* path = [self chooseCalibrationSourceWithMessage:
                               LSLocalizedString(@"ダーク（同じ設定で蓋をして撮った動画・静止画・フォルダ）を選んでください")];
    if (!path) return;
    [_darkPath release];
    _darkPath = [path copy];
    [self invalidateCalibration];
    [_statusLabel setStringValue:LSLocalizedString(@"ダークを設定しました。品質評価の開始時にマスターを作ります")];
}

- (void)chooseFlat:(id)sender {
    (void)sender;
    NSString* path = [self chooseCalibrationSourceWithMessage:
                               LSLocalizedString(@"フラット（均一な光を撮った動画・静止画・フォルダ）を選んでください")];
    if (!path) return;
    [_flatPath release];
    _flatPath = [path copy];
    [self invalidateCalibration];
    [_statusLabel setStringValue:LSLocalizedString(@"フラットを設定しました。品質評価の開始時にマスターを作ります")];
}

- (void)clearCalibration:(id)sender {
    (void)sender;
    [_darkPath release];
    _darkPath = nil;
    [_flatPath release];
    _flatPath = nil;
    [self invalidateCalibration];
}

// ---- サイドカー -----------------------------------------------------------

- (NSString*)sidecarPath {
    // **`%s` を使ってはいけない。** NSString の書式指定子 `%s` は
    // UTF-8ではなくシステムのCエンコーディングで解釈するため、
    // 日本語を含むパスが化けて、書き込みが黙って失敗する。
    // 実際、これで「サイドカーが作られない」不具合になっていた。
    NSString* input = [self inputPathString];
    if (!_inputIsSequence) return [input stringByAppendingPathExtension:@"lstk"];
    // 静止画連番はフォルダの中に置く（連番の読み込みは画像以外を無視する）。
    if (_sequenceFiles.empty()) {
        return [input stringByAppendingPathComponent:
                          [[input lastPathComponent] stringByAppendingPathExtension:@"lstk"]];
    }
    NSString* first = [NSString stringWithUTF8String:_sequenceFiles.front().c_str()];
    NSString* name = [NSString stringWithFormat:@"%@_%zu.lstk",
                                                [[first lastPathComponent] stringByDeletingPathExtension],
                                                _sequenceFiles.size()];
    return [input stringByAppendingPathComponent:name];
}

// 解析に使った設定を別ファイルで持つ。
//
// サイドカー本体（.lstk）はエンジンの形式で、参照フレームやAP変位場は入るが
// 「どの設定で解析したか」は入らない。設定が分からないまま読み込むと、
// 画面のつまみと中身が食い違ったまま再スタックできてしまう。
- (NSString*)sidecarSettingsPath {
    return [[self sidecarPath] stringByAppendingPathExtension:@"json"];
}

- (NSString*)qualityCachePath {
    return [[[self sidecarPath] stringByDeletingPathExtension] stringByAppendingPathExtension:@"lstkq"];
}

- (void)saveSidecarForCurrent {
    if (!_analysis) return;

    // ファイルサイズはエンジン側では埋まらない（解析器はファイルではなく
    // VideoSource しか知らない）。ここで入れておかないと、次に開いたときの
    // 照合が「サイズ 0 と食い違う」で必ず落ちる。
    _analysis->source_size = [self inputSizeBytes];

    try {
        stackcore::save_sidecar(std::string([[self sidecarPath] UTF8String]), *_analysis);
    } catch (const std::exception& e) {
        NSLog(@"サイドカーを保存できませんでした: %@", [NSString stringWithUTF8String:e.what()]);
        return;
    }
    NSDictionary* meta = @{
        @"signature" : [self analysisSignature],
        @"settings" : [self settingsDictionary],
        @"darkPath" : _darkPath ? _darkPath : @"",
        @"flatPath" : _flatPath ? _flatPath : @""
    };
    NSData* data = [NSJSONSerialization dataWithJSONObject:meta options:0 error:NULL];
    [data writeToFile:[self sidecarSettingsPath] atomically:YES];
}

- (void)loadSidecarForCurrent {
    NSFileManager* fm = [NSFileManager defaultManager];
    if (![fm fileExistsAtPath:[self sidecarPath]] ||
        ![fm fileExistsAtPath:[self sidecarSettingsPath]]) {
        return;
    }

    NSData* data = [NSData dataWithContentsOfFile:[self sidecarSettingsPath]];
    id meta = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL] : nil;
    if (![meta isKindOfClass:[NSDictionary class]]) return;
    NSDictionary* recordedSettings = meta[@"settings"];
    if (![recordedSettings isKindOfClass:[NSDictionary class]]) return;
    // 戻すのは、解析結果を使い回すのに必要な項目（アライメントの条件と入力の読み方）だけ。
    // Drizzle倍率・加算方式・採用率・書き出し形式などは解析と無関係なので、いまの値
    // （起動直後なら初期値）のままにする。
    NSMutableDictionary* settings = [NSMutableDictionary dictionary];
    for (NSString* key in @[ @"method", @"mode", @"apSize", @"searchRadius", @"qualityMetric",
                             @"referenceTopPercent", @"refine", @"normalizeBrightness", @"endian",
                             @"bitDepth", @"bayer", @"debayer", @"rawCfa", @"rangeStart", @"rangeEnd",
                             @"outlierK", @"minSimilarity", @"maxShift", @"minScore", @"apGradient",
                             @"apLevel" ]) {
        if (recordedSettings[key]) settings[key] = recordedSettings[key];
    }
    // 補正の素材が今と違えば、この解析結果は使えない。
    NSString* dark = meta[@"darkPath"];
    NSString* flat = meta[@"flatPath"];
    if (![(dark ? dark : @"") isEqualToString:_darkPath ? _darkPath : @""] ||
        ![(flat ? flat : @"") isEqualToString:_flatPath ? _flatPath : @""]) {
        return;
    }

    auto loaded = std::make_shared<stackcore::AnalysisData>();
    try {
        stackcore::load_sidecar(std::string([[self sidecarPath] UTF8String]), *loaded);
    } catch (const std::exception& e) {
        NSLog(@"サイドカーを読めませんでした: %@", [NSString stringWithUTF8String:e.what()]);
        return;
    }

    // 入力が変わっていないことを確かめる。
    // 古い解析結果を別の中身に当てると、黙って誤った画像が出る。
    // 解析時の設定（フレーム範囲など）を画面に戻してから、そのときのフレーム数で照合する。
    NSDictionary* before = [self settingsDictionary];
    [self applySettingsDictionary:settings includePostProcessing:NO];
    int frames = _sourceFrames;
    try {
        frames = stackcore::open_video(_inputPath, [self currentOpenOptions])->frame_count();
    } catch (const std::exception&) {
    }
    std::string message;
    if (!stackcore::matches_source(*loaded, [self inputSizeBytes], frames, _sourceWidth,
                                   _sourceHeight, _sourceChannels, message)) {
        NSLog(@"サイドカーが入力と合いません: %@", [NSString stringWithUTF8String:message.c_str()]);
        [self applySettingsDictionary:before includePostProcessing:NO];
        return;
    }

    _analysis = loaded;
    [_analysisSignature release];
    _analysisSignature = [[self analysisSignature] copy];

    NSString* recorded = meta[@"signature"];
    if ([recorded isKindOfClass:[NSString class]] &&
        ![recorded isEqualToString:_analysisSignature]) {
        // 設定を戻しても一致しないなら、信用せずに捨てる。
        _analysis.reset();
        [_analysisSignature release];
        _analysisSignature = nil;
        [self applySettingsDictionary:before includePostProcessing:NO];
        return;
    }

    // 解析時の読み方（範囲・色配列など）を戻した場合は、プレビュー用の入力も開き直す。
    if (!_openedInputSignature || ![_openedInputSignature isEqualToString:[self inputSignature]]) {
        try {
            _previewSource = std::shared_ptr<stackcore::VideoSource>(
                stackcore::open_video(_inputPath, [self currentOpenOptions]).release());
            _sourceFrames = _previewSource->frame_count();
            [_frameSlider setMaxValue:std::max(0, _sourceFrames - 1)];
            [_graph setDisplayOffset:_previewSource->original_index(0)];
            [_openedInputSignature release];
            _openedInputSignature = [[self inputSignature] copy];
            [self showSourceFrame:0];
        } catch (const std::exception&) {
        }
    }

    // サイドカーはアライメント完了後の結果なので、前段の完了状態も復元する。
    auto global = std::make_shared<stackcore::GlobalStageReport>();
    global->frames = loaded->frames;
    global->reference_index = loaded->reference_index;
    global->reference_mean = loaded->reference_mean;
    _qualityStage = std::make_shared<stackcore::GlobalStageReport>(*global);
    _globalStage = global;
    [_qualitySignature release];
    _qualitySignature = [[self qualitySignature] copy];
    [_globalSignature release];
    _globalSignature = [[self analysisSignature] copy];

    [self rebuildReferenceImage];
    [self showFrames:_analysis->frames];
    [_statusLabel
        setStringValue:LSLocalizedString(@"アライメント済みの結果を読み込みました。すぐにスタックできます")];
}

// 品質評価の結果を残す（P4）。アライメント前に閉じても、次は品質評価を省ける。
- (void)saveQualityCacheForCurrent {
    if (!_qualityStage || _inputPath.empty()) return;
    stackcore::QualityCache cache;
    cache.source_size = [self inputSizeBytes];
    cache.source_frames = _sourceFrames;
    cache.width = _sourceWidth;
    cache.height = _sourceHeight;
    cache.channels = _sourceChannels;
    cache.report = *_qualityStage;
    try {
        stackcore::save_quality_cache(std::string([[self qualityCachePath] UTF8String]), cache);
    } catch (const std::exception& e) {
        NSLog(@"品質キャッシュを保存できませんでした: %@", [NSString stringWithUTF8String:e.what()]);
        return;
    }
    NSDictionary* meta = @{@"signature" : [self qualitySignature]};
    NSData* data = [NSJSONSerialization dataWithJSONObject:meta options:0 error:NULL];
    [data writeToFile:[[self qualityCachePath] stringByAppendingPathExtension:@"json"] atomically:YES];
}

- (BOOL)loadQualityCacheForCurrent {
    NSString* path = [self qualityCachePath];
    NSString* metaPath = [path stringByAppendingPathExtension:@"json"];
    NSFileManager* fm = [NSFileManager defaultManager];
    if (![fm fileExistsAtPath:path] || ![fm fileExistsAtPath:metaPath]) return NO;
    NSData* data = [NSData dataWithContentsOfFile:metaPath];
    id meta = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL] : nil;
    if (![meta isKindOfClass:[NSDictionary class]] ||
        ![meta[@"signature"] isEqual:[self qualitySignature]]) {
        return NO;
    }
    stackcore::QualityCache cache;
    try {
        stackcore::load_quality_cache(std::string([path UTF8String]), cache);
    } catch (const std::exception&) {
        return NO;
    }
    if (cache.source_size != [self inputSizeBytes] || cache.source_frames != _sourceFrames ||
        cache.width != _sourceWidth || cache.height != _sourceHeight ||
        cache.channels != _sourceChannels) {
        return NO;
    }
    _qualityStage = std::make_shared<stackcore::GlobalStageReport>(cache.report);
    [_qualitySignature release];
    _qualitySignature = [[self qualitySignature] copy];
    [self showFrames:_qualityStage->frames];
    [_statusLabel setStringValue:LSLocalizedString(@"品質評価済みの結果を読み込みました。次はアライメントです")];
    return YES;
}

// ---- アライメントの内訳（U7） --------------------------------------------------

- (NSString*)analysisSummaryText {
    if (!_globalStage) return @"";
    const stackcore::GlobalStageReport& g = _mapReport ? _mapReport->global : *_globalStage;
    NSMutableArray* lines = [NSMutableArray array];
    const char* mode = g.mode == stackcore::AlignMode::Planet ? "惑星" : "月・太陽";
    [lines addObject:[NSString stringWithFormat:LSLocalizedString(@"対象モード: %@（参照フレーム #%d）"),
                                                LSLocalizedString([NSString stringWithUTF8String:mode]),
                                                (_previewSource ? _previewSource->original_index(g.reference_index)
                                                                : g.reference_index) + 1]];
    [lines addObject:[NSString stringWithFormat:LSLocalizedString(@"除外: 類似度不足 %d / 位置ずれ超過 %d / 外れ値 %d"),
                                                g.rejected_low_similarity, g.rejected_shift,
                                                g.rejected_outlier]];
    if (_mapReport && _mapReport->ap_frame_pairs > 0) {
        const stackcore::MapStackReport& r = *_mapReport;
        [lines addObject:[NSString stringWithFormat:LSLocalizedString(@"位置合わせ領域: %d個 × %d px（%d フレーム）"),
                                                    r.ap_count, r.ap_size, r.frames_analyzed]];
        [lines addObject:[NSString stringWithFormat:LSLocalizedString(@"一致度不足で補間: %.2f%% / 近傍との差で制限: %.2f%%"),
                                                    100.0 * r.invalid_matches / r.ap_frame_pairs,
                                                    100.0 * r.clipped_matches / r.ap_frame_pairs]];
        if (r.alignment_frame_passes > 0) {
            [lines addObject:[NSString stringWithFormat:LSLocalizedString(@"共通の位置ずれへ退避: %.2f%%（模様が一方向のとき）"),
                                                        100.0 * r.consensus_fallback_frames /
                                                            r.alignment_frame_passes]];
        }
    }
    return [lines componentsJoinedByString:@"\n"];
}

// ---- プリセット -----------------------------------------------------------

- (void)reloadPresets {
    [_presetPopup removeAllItems];
    [_presetPopup addItemWithTitle:LSLocalizedString(@"プリセット…")];
    for (NSString* name in [Presets names]) [_presetPopup addItemWithTitle:name];
}

- (void)presetSelected:(id)sender {
    (void)sender;
    if ([_presetPopup indexOfSelectedItem] <= 0) return;
    NSMutableDictionary* d = [[[Presets loadSettingsNamed:[_presetPopup titleOfSelectedItem]] mutableCopy] autorelease];
    // プリセットは処理の設定であり、特定のファイルのフレーム範囲は持ち込まない。
    [d removeObjectForKey:@"rangeStart"];
    [d removeObjectForKey:@"rangeEnd"];
    if (!d) {
        [self showError:LSLocalizedString(@"プリセットを読めませんでした")
                  title:LSLocalizedString(@"プリセット")];
        return;
    }
    [self applySettingsDictionary:d];
    [self updateControlsEnabled];
}

- (void)savePreset:(id)sender {
    (void)sender;
    NSAlert* alert = [[[NSAlert alloc] init] autorelease];
    [alert setMessageText:LSLocalizedString(@"プリセットの名前")];
    [alert addButtonWithTitle:LSLocalizedString(@"保存")];
    [alert addButtonWithTitle:LSLocalizedString(@"やめる")];
    NSTextField* field =
        [[[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 240, 24)] autorelease];
    [field setStringValue:LSLocalizedString(@"マイ設定")];
    [alert setAccessoryView:field];
    [[alert window] setInitialFirstResponder:field];
    if ([alert runModal] != NSAlertFirstButtonReturn) return;

    NSString* error = nil;
    if (![Presets saveSettings:[self settingsDictionary]
                          name:[field stringValue]
                         error:&error]) {
        [self showError:error ? error : LSLocalizedString(@"保存できませんでした")
                  title:LSLocalizedString(@"プリセット")];
        return;
    }
    [self reloadPresets];
    [_statusLabel setStringValue:[NSString
                                     stringWithFormat:
                                         LSLocalizedString(@"プリセット「%@」を保存しました"),
                                                            [field stringValue]]];
}

@end
