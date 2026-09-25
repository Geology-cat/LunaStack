#import "MainWindowController_Private.h"

#include <algorithm>
#include <cmath>

#include "stackcore/map_pipeline.hpp"
#include "stackcore/video_source.hpp"
#include "stackcore/wavelet.hpp"

@implementation MainWindowController (Settings)

// ---- 設定の読み取りと無効化 -----------------------------------------------

// 入力の解釈（バイトオーダー・ビット深度）。
// **解析結果に効く。** 変えたら解析はやり直しになる。
- (stackcore::OpenOptions)currentOpenOptions {
    stackcore::OpenOptions options;
    const stackcore::ByteOrder orders[] = {stackcore::ByteOrder::Auto, stackcore::ByteOrder::Little,
                                           stackcore::ByteOrder::Big};
    options.endian = orders[[_endianPopup indexOfSelectedItem]];
    const int depths[] = {0, 12, 14};
    options.bit_depth_override = depths[[_depthPopup indexOfSelectedItem]];
    return options;
}

- (void)inputInterpretationChanged:(id)sender {
    (void)sender;
    _bannerDismissed = NO;
    // 読み方が変われば1枚目の見え方も変わる。開き直して確かめられるようにする。
    if (_currentIndex >= 0) [self selectQueueIndex:_currentIndex];
    [self updateControlsEnabled];
}

- (stackcore::MapStackSettings)currentSettings {
    stackcore::MapStackSettings settings;
    const stackcore::AlignMode modes[] = {stackcore::AlignMode::Auto, stackcore::AlignMode::Planet,
                                          stackcore::AlignMode::Lunar};
    settings.global.align.mode = modes[[_modeSegment selectedSegment]];
    settings.global.limit = _frameLimit;
    settings.global.quality_metric =
        [_qualityMetricPopup indexOfSelectedItem] == 1
            ? stackcore::QualityMetric::FrequencyBandPowerRatio
            : stackcore::QualityMetric::GradientEnergy;
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
    settings.stack_mode = stackModes[[_stackModePopup indexOfSelectedItem]];
    settings.reference_passes = ([_refineCheck state] == NSControlStateValueOn) ? 2 : 1;

    settings.ap.ap_size = LSApSizeAt([_apSizePopup indexOfSelectedItem]);
    settings.ap.use_manual_points = _manualPointsActive ? true : false;
    settings.ap.manual_points = _manualPoints;
    settings.local.search_radius = static_cast<int>([_searchRadiusSlider doubleValue] + 0.5);

    settings.drizzle_scale = LSDrizzleScaleAt([_drizzleSegment selectedSegment]);
    settings.pixfrac = [_pixfracSlider doubleValue];
    return settings;
}

// 品質評価結果を使い回してよいかを判断するための指紋。
// 位置合わせの設定は入れない。領域の大きさを変えても品質は再計算不要である。
- (NSString*)qualitySignature {
    return [NSString
        stringWithFormat:@"path=%@;quality=%ld;limit=%d;endian=%ld;depth=%ld;",
                         [self inputPathString], (long)[_qualityMetricPopup indexOfSelectedItem],
                         _frameLimit, (long)[_endianPopup indexOfSelectedItem],
                         (long)[_depthPopup indexOfSelectedItem]];
}

// アライメント結果を使い回してよいかを判断するための指紋。
//
// **ここに入れる項目と入れない項目の区別が、このアプリの速さそのもの**である。
//   入れる  : 解析結果そのものが変わるもの（AP・参照・追跡）
//   入れない: 加算だけをやり直せば済むもの（AP別の選択率・Drizzle・低メモリ）
- (NSString*)analysisSignature {
    NSMutableString* s = [NSMutableString string];
    [s appendFormat:@"path=%@;", [self inputPathString]];
    [s appendFormat:@"method=%ld;", (long)[_methodPopup indexOfSelectedItem]];
    [s appendFormat:@"mode=%ld;", (long)[_modeSegment selectedSegment]];
    [s appendFormat:@"apsize=%ld;", (long)[_apSizePopup indexOfSelectedItem]];
    [s appendFormat:@"radius=%.0f;", [_searchRadiusSlider doubleValue]];
    [s appendFormat:@"reftop=%.2f;", [_topSlider doubleValue]];
    [s appendFormat:@"quality=%ld;", (long)[_qualityMetricPopup indexOfSelectedItem]];
    [s appendFormat:@"passes=%d;", ([_refineCheck state] == NSControlStateValueOn) ? 2 : 1];
    [s appendFormat:@"limit=%d;", _frameLimit];
    [s appendFormat:@"endian=%ld;depth=%ld;", (long)[_endianPopup indexOfSelectedItem],
                    (long)[_depthPopup indexOfSelectedItem]];
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
    [self updateControlsEnabled];
    [self updateNamePreview];
}

- (void)topChanged:(id)sender {
    (void)sender;
    [_topValue setStringValue:[NSString stringWithFormat:@"%.0f %%", [_topSlider doubleValue]]];
    [_graph setCutPercent:[_topSlider doubleValue]];
    [self updateControlsEnabled];
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
    [self updateNamePreview];
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
    [self updateNamePreview];
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
    [self updateNamePreview];
}

- (void)updateDrizzleEstimate {
    const double scale = LSDrizzleScaleAt([_drizzleSegment selectedSegment]);
    if (scale <= 1.0 || _inputPath.empty()) {
        [_drizzleEstimate setStringValue:@""];
        return;
    }
    try {
        const std::unique_ptr<stackcore::VideoSource> source =
            stackcore::open_video(_inputPath, [self currentOpenOptions]);
        const int w = static_cast<int>(source->width() * scale);
        const int h = static_cast<int>(source->height() * scale);
        // 出力＋重みで float 2面ぶんを持つ。
        const double mb = static_cast<double>(w) * h * _sourceChannels * 4 * 2 / (1024.0 * 1024.0);
        [_drizzleEstimate
            setStringValue:[NSString
                               stringWithFormat:
                                   LSLocalizedString(@"出力 %d×%d ／ 作業メモリ約 %.0f MB"),
                               w, h, mb]];
    } catch (const std::exception&) {
        [_drizzleEstimate setStringValue:@""];
    }
}

- (void)graphModeChanged:(id)sender {
    (void)sender;
    [_graph setSortedByQuality:[_graphMode selectedSegment] == 1];
}

- (void)qualityGraphView:(QualityGraphView*)view didChangeCutPercent:(double)percent {
    (void)view;
    double p = percent;
    if (p < [_topSlider minValue]) p = [_topSlider minValue];
    if (p > [_topSlider maxValue]) p = [_topSlider maxValue];
    [_topSlider setDoubleValue:p];
    [_topValue setStringValue:[NSString stringWithFormat:@"%.0f %%", p]];
    [self updateControlsEnabled];
}

- (void)zoomChanged:(id)sender {
    (void)sender;
    const double zooms[] = {0.0, 1.0, 2.0, 4.0};
    [_preview setZoom:zooms[[_zoomControl selectedSegment]]];
}

- (void)apDisplayChanged:(id)sender {
    (void)sender;
    [_preview setShowAlignmentPoints:[_apShowCheck state] == NSControlStateValueOn];
    [_preview setApHeatmap:[_apHeatCheck state] == NSControlStateValueOn];
    [_preview setApEditing:[_apEditCheck state] == NSControlStateValueOn];
    if ([_apEditCheck state] == NSControlStateValueOn) {
        [[self window] makeFirstResponder:_preview];
    }
}

- (void)updateControlsEnabled {
    const BOOL hasFile = !_inputPath.empty();
    const BOOL globalOnly = [_methodPopup indexOfSelectedItem] == 1;
    const BOOL qualityOk = [self qualityUsable];
    const BOOL alignmentOk = [self alignmentUsable];

    // **実行中は選択を変えさせない。**
    // 変えると _inputPath が差し替わり、走り終わった仕事の解析結果が
    // 「今選んでいる別のファイル」のサイドカーとして書き出されてしまう。
    // しかも保存時にファイルサイズを今のファイルのもので上書きするので、
    // 枚数や大きさがたまたま同じ別撮りだと、次に開いたときの照合を
    // すり抜けて黙って誤った画像が出る。
    [_queueTable setEnabled:!_running];

    [_qualityButton setEnabled:hasFile && !_running];
    [_alignButton setEnabled:hasFile && qualityOk && !_running];
    [_stackButton setEnabled:hasFile && alignmentOk && !_running];
    [_clearButton setEnabled:([_items count] > 0) && !_running];
    [_cancelButton setEnabled:_running];
    [_saveButton setEnabled:(_displayed != nullptr) && !_running];

    [_apSizePopup setEnabled:!globalOnly];
    [_apTopSlider setEnabled:!globalOnly];
    [_selectionModeSegment setEnabled:!globalOnly];
    [_qualityMetricPopup setEnabled:!_running];
    [_stackModePopup setEnabled:!globalOnly && !_running];
    [_refineCheck setEnabled:!globalOnly];
    [_searchRadiusSlider setEnabled:!globalOnly];
    [_apEditCheck setEnabled:!globalOnly];
    [_methodPopup setEnabled:!_running];

    // 各工程の完了状態を、押す前に分かるようにする。
    [_qualityButton setTitle:LSLocalizedString(qualityOk ? @"品質を再評価" : @"品質評価")];
    [_alignButton setTitle:LSLocalizedString(alignmentOk ? @"再アライメント"
                                                    : @"アライメント")];
    [_stackButton setTitle:LSLocalizedString(_stacked ? @"再スタック" : @"スタック")];
    if (_currentIndex >= 0 && !_running) {
        QueueItem* item = _items[static_cast<NSUInteger>(_currentIndex)];
        if ([item state] != QueueItemStateError && [item state] != QueueItemStateStacked) {
            [item setState:alignmentOk ? QueueItemStateAnalyzed
                                      : (qualityOk ? QueueItemStateQualityEvaluated
                                                   : QueueItemStatePending)];
            [_queueTable reloadData];
            [_queueTable selectRowIndexes:[NSIndexSet indexSetWithIndex:
                                                          static_cast<NSUInteger>(_currentIndex)]
                     byExtendingSelection:NO];
        }
    }
}

// ---- サイドカー -----------------------------------------------------------

// 入力パスを NSString で得る。
// パスに日本語が入りうるので、常に UTF-8 として変換する。
- (NSString*)inputPathString {
    return [NSString stringWithUTF8String:_inputPath.c_str()];
}

- (NSString*)sidecarPath {
    // **`%s` を使ってはいけない。** NSString の書式指定子 `%s` は
    // UTF-8ではなくシステムのCエンコーディングで解釈するため、
    // 日本語を含むパスが化けて、書き込みが黙って失敗する。
    // 実際、これで「サイドカーが作られない」不具合になっていた。
    return [[self inputPathString] stringByAppendingPathExtension:@"lstk"];
}

// 解析に使った設定を別ファイルで持つ。
//
// サイドカー本体（.lstk）はエンジンの形式で、参照フレームやAP変位場は入るが
// 「どの設定で解析したか」は入らない。設定が分からないまま読み込むと、
// 画面のつまみと中身が食い違ったまま再スタックできてしまう。
- (NSString*)sidecarSettingsPath {
    return [[self sidecarPath] stringByAppendingPathExtension:@"json"];
}

- (void)saveSidecarForCurrent {
    if (!_analysis) return;

    // ファイルサイズはエンジン側では埋まらない（解析器はファイルではなく
    // VideoSource しか知らない）。ここで入れておかないと、次に開いたときの
    // 照合が「サイズ 0 と食い違う」で必ず落ちる。
    NSDictionary* attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:
                                                              [self inputPathString]
                                                                          error:NULL];
    _analysis->source_size = [attrs[NSFileSize] longLongValue];

    try {
        stackcore::save_sidecar(std::string([[self sidecarPath] UTF8String]), *_analysis);
    } catch (const std::exception& e) {
        NSLog(@"サイドカーを保存できませんでした: %@", [NSString stringWithUTF8String:e.what()]);
        return;
    }
    NSDictionary* meta = @{
        @"signature" : [self analysisSignature],
        @"settings" : [self settingsDictionary]
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
    NSDictionary* settings = meta[@"settings"];
    if (![settings isKindOfClass:[NSDictionary class]]) return;

    auto loaded = std::make_shared<stackcore::AnalysisData>();
    try {
        stackcore::load_sidecar(std::string([[self sidecarPath] UTF8String]), *loaded);
    } catch (const std::exception& e) {
        NSLog(@"サイドカーを読めませんでした: %@", [NSString stringWithUTF8String:e.what()]);
        return;
    }

    // 入力が変わっていないことを確かめる。
    // 古い解析結果を別の中身に当てると、黙って誤った画像が出る。
    NSDictionary* attrs = [fm attributesOfItemAtPath:
                                  [NSString stringWithUTF8String:_inputPath.c_str()]
                                               error:NULL];
    const long long size = [attrs[NSFileSize] longLongValue];
    std::string message;
    if (!stackcore::matches_source(*loaded, size, _sourceFrames, _sourceWidth, _sourceHeight,
                                   _sourceChannels, message)) {
        NSLog(@"サイドカーが入力と合いません: %@", [NSString stringWithUTF8String:message.c_str()]);
        return;
    }

    // 解析時の設定を画面に戻してから、指紋を照合する。
    // 仕上げのつまみは戻さない（解析とは無関係で、触っていた値を奪わない）。
    [self applySettingsDictionary:settings includePostProcessing:NO];
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
        return;
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

// ---- プリセット -----------------------------------------------------------

// プリセットとサイドカーの両方で使う、設定一式の辞書表現。
- (NSDictionary*)settingsDictionary {
    NSMutableArray* sharpen = [NSMutableArray array];
    NSMutableArray* denoise = [NSMutableArray array];
    for (int j = 0; j < kWaveletLayers; ++j) {
        [sharpen addObject:@([_sharpenSliders[j] doubleValue])];
        [denoise addObject:@([_denoiseSliders[j] doubleValue])];
    }
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
        @"lowMemory" : @([_lowMemoryCheck state] == NSControlStateValueOn),
        @"drizzle" : @([_drizzleSegment selectedSegment]),
        @"pixfrac" : @([_pixfracSlider doubleValue]),
        @"sharpen" : sharpen,
        @"denoise" : denoise,
        @"format" : @([_formatPopup indexOfSelectedItem]),
        @"endian" : @([_endianPopup indexOfSelectedItem]),
        @"bitDepth" : @([_depthPopup indexOfSelectedItem]),
    };
}

- (void)applySettingsDictionary:(NSDictionary*)d {
    [self applySettingsDictionary:d includePostProcessing:YES];
}

// includePostProcessing が NO のとき、ウェーブレットのつまみは触らない。
//
// サイドカーの読み込みで使う。解析結果に付いてきた設定を戻すのは
// 「画面と中身を一致させる」ためであって、**仕上げは解析と無関係**である。
// 触っていた仕上げが、ファイルを開き直しただけで勝手に動くのは
// 「後処理は非破壊」（UI設計書 §1.5）に反する。
- (void)applySettingsDictionary:(NSDictionary*)d includePostProcessing:(BOOL)includePost {
    if (d[@"method"]) [_methodPopup selectItemAtIndex:[d[@"method"] integerValue]];
    if (d[@"mode"]) [_modeSegment setSelectedSegment:[d[@"mode"] integerValue]];
    if (d[@"apSize"]) [_apSizePopup selectItemAtIndex:[d[@"apSize"] integerValue]];
    if (d[@"searchRadius"]) [_searchRadiusSlider setDoubleValue:[d[@"searchRadius"] doubleValue]];
    if (d[@"qualityMetric"]) {
        [_qualityMetricPopup selectItemAtIndex:[d[@"qualityMetric"] integerValue]];
    }
    if (d[@"referenceTopPercent"]) {
        [_topSlider setDoubleValue:[d[@"referenceTopPercent"] doubleValue]];
    }
    if (d[@"refine"]) {
        [_refineCheck setState:[d[@"refine"] boolValue] ? NSControlStateValueOn
                                                        : NSControlStateValueOff];
    }
    if (d[@"apTopPercent"]) _apTopPercentSetting = [d[@"apTopPercent"] doubleValue];
    if (d[@"apTopCount"]) _apTopCountSetting = [d[@"apTopCount"] intValue];
    _selectionUsesCount = d[@"selectionMode"] && [d[@"selectionMode"] integerValue] == 1;
    [_selectionModeSegment setSelectedSegment:_selectionUsesCount ? 1 : 0];
    [self refreshSelectionControl];
    if (d[@"normalizeBrightness"]) {
        [_normalizeCheck setState:[d[@"normalizeBrightness"] boolValue]
                                      ? NSControlStateValueOn
                                      : NSControlStateValueOff];
    }
    if (d[@"stackMode"]) [_stackModePopup selectItemAtIndex:[d[@"stackMode"] integerValue]];
    if (d[@"lowMemory"]) {
        [_lowMemoryCheck setState:[d[@"lowMemory"] boolValue] ? NSControlStateValueOn
                                                              : NSControlStateValueOff];
    }
    if (d[@"drizzle"]) [_drizzleSegment setSelectedSegment:[d[@"drizzle"] integerValue]];
    if (d[@"pixfrac"]) [_pixfracSlider setDoubleValue:[d[@"pixfrac"] doubleValue]];
    if (d[@"format"]) [_formatPopup selectItemAtIndex:[d[@"format"] integerValue]];
    if (d[@"endian"]) [_endianPopup selectItemAtIndex:[d[@"endian"] integerValue]];
    if (d[@"bitDepth"]) [_depthPopup selectItemAtIndex:[d[@"bitDepth"] integerValue]];

    NSArray* sharpen = includePost ? d[@"sharpen"] : nil;
    NSArray* denoise = includePost ? d[@"denoise"] : nil;
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

    [self topChanged:nil];
    [self refreshSelectionControl];
    [self searchRadiusChanged:nil];
    [self drizzleChanged:nil];
    [self waveletChanged:nil];
}

- (void)reloadPresets {
    [_presetPopup removeAllItems];
    [_presetPopup addItemWithTitle:LSLocalizedString(@"プリセット…")];
    for (NSString* name in [Presets names]) [_presetPopup addItemWithTitle:name];
}

- (void)presetSelected:(id)sender {
    (void)sender;
    if ([_presetPopup indexOfSelectedItem] <= 0) return;
    NSDictionary* d = [Presets loadSettingsNamed:[_presetPopup titleOfSelectedItem]];
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
