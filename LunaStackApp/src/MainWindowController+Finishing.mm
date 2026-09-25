#import "MainWindowController_Private.h"

#include <algorithm>
#include <cmath>

#include "stackcore/metadata.hpp"

namespace {

// ドラッグ中に縮小版で追従させる大きさの目安（画素数）。これより小さければ常に本解像度。
constexpr double kDraftPixels = 3.0e6;

// 仕上げを掛けない（変化なし）ときの既定の配分。連動で強さを上げたときの形。
const double kLinkedProfile[kWaveletLayers] = {7.0, 3.0, 1.0, 0.0, 0.0, 0.0};

double ParseField(NSTextField* field) {
    NSScanner* scanner = [NSScanner scannerWithString:[field stringValue]];
    double v = 0.0;
    if (![scanner scanDouble:&v] || !std::isfinite(v)) return 0.0;
    return std::max(-64.0, std::min(64.0, v));
}

// 2×2の平均で半分に縮める（ドラッグ中の下書き用）。
std::shared_ptr<stackcore::FrameBuffer> HalfSize(const stackcore::FrameBuffer& src) {
    const int w = std::max(1, src.width() / 2), h = std::max(1, src.height() / 2);
    auto out = std::make_shared<stackcore::FrameBuffer>(w, h, src.channels());
    for (int c = 0; c < src.channels(); ++c) {
        for (int y = 0; y < h; ++y) {
            const float* a = src.row(c, std::min(src.height() - 1, 2 * y));
            const float* b = src.row(c, std::min(src.height() - 1, 2 * y + 1));
            float* d = out->row(c, y);
            for (int x = 0; x < w; ++x) {
                const int x0 = std::min(src.width() - 1, 2 * x), x1 = std::min(src.width() - 1, 2 * x + 1);
                d[x] = 0.25f * (a[x0] + a[x1] + b[x0] + b[x1]);
            }
        }
    }
    out->set_source_bit_depth(src.source_bit_depth());
    return out;
}

}  // namespace

@implementation MainWindowController (Finishing)

// ---- 設定の読み取り -----------------------------------------------------------

- (std::vector<stackcore::WaveletLayerParams>)waveletParams {
    std::vector<stackcore::WaveletLayerParams> params(kWaveletLayers);
    for (int j = 0; j < kWaveletLayers; ++j) {
        params[static_cast<std::size_t>(j)].sharpen = [_sharpenSliders[j] doubleValue];
        params[static_cast<std::size_t>(j)].denoise = [_denoiseSliders[j] doubleValue];
    }
    return params;
}

- (stackcore::FinishingSettings)currentFinishingSettings {
    stackcore::FinishingSettings s;
    s.channels.red_dx = ParseField(_channelFields[0]);
    s.channels.red_dy = ParseField(_channelFields[1]);
    s.channels.blue_dx = ParseField(_channelFields[2]);
    s.channels.blue_dy = ParseField(_channelFields[3]);
    s.wavelet = [self waveletParams];
    s.dering = [_deringSlider doubleValue];
    for (int c = 0; c < 3; ++c) s.color.gain[c] = [_gainSliders[c] doubleValue];
    s.color.saturation = [_saturationSlider doubleValue];
    s.stretch = [_toneCheck state] == NSControlStateValueOn;
    s.black = [_blackSlider doubleValue];
    s.white = std::max(s.black + 0.01, [_whiteSlider doubleValue]);
    s.gamma = [_gammaSlider doubleValue];
    s.geometry.rotate_quarter_turns = _rotationTurns;
    s.geometry.flip_horizontal = [_flipHCheck state] == NSControlStateValueOn;
    s.geometry.flip_vertical = [_flipVCheck state] == NSControlStateValueOn;
    if ([_cropCheck state] == NSControlStateValueOn && _cropRect.size.width > 0 && _stacked) {
        s.geometry.crop = true;
        s.geometry.crop_x = static_cast<int>(_cropRect.origin.x);
        s.geometry.crop_y = static_cast<int>(_cropRect.origin.y);
        s.geometry.crop_width = static_cast<int>(_cropRect.size.width);
        s.geometry.crop_height = static_cast<int>(_cropRect.size.height);
    }
    return s;
}

// 新しいスタックを始める・入力を変えるときに、前の結果と描画待ちを捨てる。
- (void)resetFinishingForNewStack {
    ++_renderGeneration;
    _stacked.reset();
    _displayed.reset();
    _finishing.reset();
    _finishingDraft.reset();
    [_stackedInfo release];
    _stackedInfo = nil;
    _stackedFrames.clear();
    _cropRect = NSZeroRect;
    [_cropLabel setStringValue:LSLocalizedString(@"切り抜く範囲はまだありません")];
    [_preview setCropOverlay:NSZeroRect];
}

// ---- 描画 -----------------------------------------------------------------

- (void)applyWavelet {
    [self requestFinishingRender:NO];
}

// 仕上げ済みの画像を作る。重い処理なので直列キューで行い、終わったものから最新だけを出す。
// draft が YES なら、大きな画像では縮小版で先に追従する（UI設計書 §4.5）。
- (void)requestFinishingRender:(BOOL)draft {
    if (!_finishing || !_stacked) return;
    const stackcore::FinishingSettings settings = [self currentFinishingSettings];
    const long generation = ++_renderGeneration;
    const bool useDraft = draft && static_cast<double>(_stacked->width()) * _stacked->height() > kDraftPixels;

    std::shared_ptr<stackcore::FinishingPipeline> pipeline = _finishing;
    stackcore::FinishingSettings job = settings;
    if (useDraft) {
        if (!_finishingDraft) {
            _finishingDraft = std::make_shared<stackcore::FinishingPipeline>();
            _finishingDraft->set_input(HalfSize(*_stacked), kWaveletLayers);
        }
        pipeline = _finishingDraft;
        // 半分の解像度では、細かい順に1段ずつずれた層が同じ大きさの構造に当たる。
        for (int j = 0; j < kWaveletLayers; ++j) {
            job.wavelet[static_cast<std::size_t>(j)] =
                j + 1 < kWaveletLayers ? settings.wavelet[static_cast<std::size_t>(j + 1)]
                                       : stackcore::WaveletLayerParams();
        }
        job.channels.red_dx /= 2.0;
        job.channels.red_dy /= 2.0;
        job.channels.blue_dx /= 2.0;
        job.channels.blue_dy /= 2.0;
        if (job.geometry.crop) {
            job.geometry.crop_x /= 2;
            job.geometry.crop_y /= 2;
            job.geometry.crop_width = std::max(1, job.geometry.crop_width / 2);
            job.geometry.crop_height = std::max(1, job.geometry.crop_height / 2);
        }
    }

    MainWindowController* controller = self;
    dispatch_async(_finishQueue, ^{
        auto out = std::make_shared<stackcore::FrameBuffer>();
        std::string error;
        try {
            pipeline->render(job, *out);
        } catch (const std::exception& e) {
            error = e.what();
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            [controller finishedRender:out generation:generation draft:useDraft error:error];
        });
    });
}

- (void)finishedRender:(std::shared_ptr<stackcore::FrameBuffer>)out
            generation:(long)generation
                 draft:(bool)draft
                 error:(const std::string&)error {
    if (generation != _renderGeneration) return;  // 後から新しい要求が来ている
    if (!error.empty()) {
        [_statusLabel setStringValue:[NSString stringWithUTF8String:error.c_str()]];
        return;
    }
    if (!draft) _displayed = out;
    if ([_viewModeSegment selectedSegment] != 2) return;
    if ([_waveletPreviewCheck state] == NSControlStateValueOn) {
        [_preview showSharedFrame:out];
    }
    [self updateApOverlay];
}

// 仕上げを同期して描く（書き出しと自己検証用）。
- (std::shared_ptr<stackcore::FrameBuffer>)renderFinishingNow {
    if (!_finishing) return nullptr;
    const stackcore::FinishingSettings settings = [self currentFinishingSettings];
    std::shared_ptr<stackcore::FinishingPipeline> pipeline = _finishing;
    auto out = std::make_shared<stackcore::FrameBuffer>();
    __block std::string error;
    stackcore::FrameBuffer* target = out.get();
    dispatch_sync(_finishQueue, ^{
        try {
            pipeline->render(settings, *target);
        } catch (const std::exception& e) {
            error = e.what();
        }
    });
    if (!error.empty()) throw std::runtime_error(error);
    return out;
}

- (void)showFinishedOrStacked {
    if (!_stacked) return;
    const BOOL showEffect = [_waveletPreviewCheck state] == NSControlStateValueOn;
    if (showEffect && _displayed) {
        [_preview showSharedFrame:_displayed];
    } else {
        [_preview showSharedFrame:_stacked];
        if (showEffect) [self requestFinishingRender:NO];
    }
    [self updateApOverlay];
}

// ---- つまみの操作 -----------------------------------------------------------

// ドラッグ中か（離したら本解像度で描き直す）。
- (BOOL)sliderIsDragging {
    const NSEventType type = [[NSApp currentEvent] type];
    return type == NSEventTypeLeftMouseDragged || type == NSEventTypeLeftMouseDown;
}

- (void)updateFinishingValueLabels {
    for (int j = 0; j < kWaveletLayers; ++j) {
        [_sharpenValues[j] setStringValue:[NSString stringWithFormat:@"%.2f", [_sharpenSliders[j] doubleValue]]];
        [_denoiseValues[j] setStringValue:[NSString stringWithFormat:@"%.2f", [_denoiseSliders[j] doubleValue]]];
        [_sharpenSliders[j] setEnabled:[_linkedCheck state] != NSControlStateValueOn];
    }
    [_linkedSlider setEnabled:[_linkedCheck state] == NSControlStateValueOn];
    [_linkedValue setStringValue:[NSString stringWithFormat:@"%.2f", [_linkedSlider doubleValue]]];
    [_deringValue setStringValue:[NSString stringWithFormat:@"%.2f", [_deringSlider doubleValue]]];
    for (int c = 0; c < 3; ++c) {
        [_gainValues[c] setStringValue:[NSString stringWithFormat:@"%.3f", [_gainSliders[c] doubleValue]]];
    }
    [_saturationValue setStringValue:[NSString stringWithFormat:@"%.2f", [_saturationSlider doubleValue]]];
    [_blackValue setStringValue:[NSString stringWithFormat:@"%.3f", [_blackSlider doubleValue]]];
    [_whiteValue setStringValue:[NSString stringWithFormat:@"%.3f", [_whiteSlider doubleValue]]];
    [_gammaValue setStringValue:[NSString stringWithFormat:@"%.2f", [_gammaSlider doubleValue]]];
    const BOOL tone = [_toneCheck state] == NSControlStateValueOn;
    for (NSControl* c in @[ _blackSlider, _whiteSlider, _gammaSlider ]) [c setEnabled:tone];
    NSString* rotation = _rotationTurns == 0 ? LSLocalizedString(@"回転なし")
                                             : [NSString stringWithFormat:LSLocalizedString(@"右へ %d°"),
                                                                          _rotationTurns * 90];
    [_rotationLabel setStringValue:rotation];
    if (_cropRect.size.width > 0) {
        [_cropLabel setStringValue:[NSString stringWithFormat:LSLocalizedString(@"x %.0f, y %.0f から %.0f×%.0f px"),
                                                              _cropRect.origin.x, _cropRect.origin.y,
                                                              _cropRect.size.width, _cropRect.size.height]];
    }
}

// 連動の配分（最後に連動を入れたときの「強調−1」の比）。
- (void)captureLinkedProfile:(double*)profile {
    double sum = 0.0;
    for (int j = 0; j < kWaveletLayers; ++j) {
        profile[j] = [_sharpenSliders[j] doubleValue] - 1.0;
        sum += std::fabs(profile[j]);
    }
    if (sum < 1e-9) {
        for (int j = 0; j < kWaveletLayers; ++j) profile[j] = kLinkedProfile[j];
    }
}

- (void)waveletChanged:(id)sender {
    static double profile[kWaveletLayers] = {7.0, 3.0, 1.0, 0.0, 0.0, 0.0};
    if (sender == _linkedCheck) {
        if ([_linkedCheck state] == NSControlStateValueOn) {
            // 入れた瞬間の配分を基準にし、強さ1.0＝いまの見た目から始める。
            [self captureLinkedProfile:profile];
            [_linkedSlider setDoubleValue:1.0];
        }
    } else if (sender == _linkedSlider && [_linkedCheck state] == NSControlStateValueOn) {
        const double amount = [_linkedSlider doubleValue];
        for (int j = 0; j < kWaveletLayers; ++j) {
            const double v = 1.0 + amount * profile[j];
            [_sharpenSliders[j] setDoubleValue:std::max(0.0, std::min(kWaveletGuiSharpenMaximum, v))];
        }
    }
    [self updateFinishingValueLabels];
    [self requestFinishingRender:[self sliderIsDragging]];
    if (!_restoringSettings && ![self sliderIsDragging]) [self persistSettings];
}

- (void)finishingChanged:(id)sender {
    (void)sender;
    [self updateFinishingValueLabels];
    [self requestFinishingRender:[self sliderIsDragging]];
    if (!_restoringSettings && ![self sliderIsDragging]) [self persistSettings];
}

- (void)waveletPreviewChanged:(id)sender {
    (void)sender;
    if (!_stacked) return;
    [_viewModeSegment setSelectedSegment:2];
    const BOOL showEffect = [_waveletPreviewCheck state] == NSControlStateValueOn;
    [self showFinishedOrStacked];
    [_statusLabel
        setStringValue:LSLocalizedString(showEffect ? @"仕上げの効果ありのプレビュー"
                                              : @"仕上げの効果なしのプレビュー（スタックそのまま）")];
    [self updateFrameInfoLabel];
}

- (void)resetWavelet:(id)sender {
    (void)sender;
    for (int j = 0; j < kWaveletLayers; ++j) {
        [_sharpenSliders[j] setDoubleValue:1.0];
        [_denoiseSliders[j] setDoubleValue:0.0];
    }
    [_linkedCheck setState:NSControlStateValueOff];
    [_deringSlider setDoubleValue:0.0];
    [self waveletChanged:nil];
}

// ---- 自動の調整 -------------------------------------------------------------

- (void)autoChannelAlign:(id)sender {
    (void)sender;
    if (!_stacked) return;
    if (_stacked->channels() != 3) {
        [_statusLabel setStringValue:LSLocalizedString(@"チャンネル合わせはカラー画像にだけ使えます")];
        return;
    }
    std::shared_ptr<stackcore::FrameBuffer> input = _stacked;
    MainWindowController* controller = self;
    [_statusLabel setStringValue:LSLocalizedString(@"RGBのずれを推定しています…")];
    dispatch_async(_finishQueue, ^{
        const stackcore::ChannelOffsets o = stackcore::estimate_channel_offsets(*input);
        dispatch_async(dispatch_get_main_queue(), ^{
            [controller applyChannelOffsets:o];
        });
    });
}

- (void)applyChannelOffsets:(stackcore::ChannelOffsets)o {
    const double v[4] = {o.red_dx, o.red_dy, o.blue_dx, o.blue_dy};
    for (int i = 0; i < 4; ++i) [_channelFields[i] setStringValue:[NSString stringWithFormat:@"%.2f", v[i]]];
    [_statusLabel setStringValue:[NSString stringWithFormat:LSLocalizedString(@"RGBのずれ: R (%.2f, %.2f) / B (%.2f, %.2f) px"),
                                                            v[0], v[1], v[2], v[3]]];
    [self finishingChanged:nil];
}

- (void)resetChannelAlign:(id)sender {
    (void)sender;
    for (int i = 0; i < 4; ++i) [_channelFields[i] setStringValue:@"0.00"];
    [self finishingChanged:nil];
}

- (void)autoWhiteBalance:(id)sender {
    (void)sender;
    if (!_stacked || _stacked->channels() != 3) {
        [_statusLabel setStringValue:LSLocalizedString(@"ホワイトバランスはカラー画像にだけ使えます")];
        return;
    }
    std::shared_ptr<stackcore::FrameBuffer> input = _stacked;
    const stackcore::ChannelOffsets offsets = [self currentFinishingSettings].channels;
    MainWindowController* controller = self;
    dispatch_async(_finishQueue, ^{
        // チャンネル合わせの後の画像で推定する（ずれた縁の色に引きずられない）。
        stackcore::FrameBuffer aligned;
        stackcore::shift_channels(*input, offsets, aligned);
        double gains[3];
        stackcore::estimate_white_balance(aligned, gains);
        const double r = gains[0], b = gains[2];
        dispatch_async(dispatch_get_main_queue(), ^{
            [controller applyGainsRed:r blue:b];
        });
    });
}

- (void)applyGainsRed:(double)r blue:(double)b {
    [_gainSliders[0] setDoubleValue:r];
    [_gainSliders[1] setDoubleValue:1.0];
    [_gainSliders[2] setDoubleValue:b];
    [self finishingChanged:nil];
}

- (void)resetColor:(id)sender {
    (void)sender;
    for (int c = 0; c < 3; ++c) [_gainSliders[c] setDoubleValue:1.0];
    [_saturationSlider setDoubleValue:1.0];
    [self finishingChanged:nil];
}

// 黒点・白点を画像の分位点に合わせる（背景の少し上から、明部の飽和の少し手前まで）。
- (void)autoTone:(id)sender {
    (void)sender;
    if (!_finishing) return;
    stackcore::FinishingSettings settings = [self currentFinishingSettings];
    settings.stretch = false;
    settings.geometry = stackcore::Geometry();
    std::shared_ptr<stackcore::FinishingPipeline> pipeline = _finishing;
    MainWindowController* controller = self;
    dispatch_async(_finishQueue, ^{
        stackcore::FrameBuffer out;
        pipeline->render(settings, out);
        std::vector<float> values;
        values.reserve(static_cast<std::size_t>(out.width()) * out.height());
        for (int y = 0; y < out.height(); ++y) {
            for (int x = 0; x < out.width(); ++x) {
                float v = 0.0f;
                for (int c = 0; c < out.channels(); ++c) v = std::max(v, out.row(c, y)[x]);
                values.push_back(v);
            }
        }
        const auto pick = [&values](double p) {
            const std::size_t k = std::min(values.size() - 1, static_cast<std::size_t>(p * (values.size() - 1)));
            std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
            return static_cast<double>(values[k]);
        };
        const double black = values.empty() ? 0.0 : pick(0.02);
        const double white = values.empty() ? 1.0 : pick(0.9995);
        dispatch_async(dispatch_get_main_queue(), ^{
            [controller applyToneBlack:black white:white];
        });
    });
}

- (void)applyToneBlack:(double)black white:(double)white {
    [_toneCheck setState:NSControlStateValueOn];
    [_blackSlider setDoubleValue:std::max(0.0, std::min(0.5, black))];
    [_whiteSlider setDoubleValue:std::max(black + 0.02, std::min(1.0, white))];
    [self finishingChanged:nil];
}

// ---- 向き・切り抜き -----------------------------------------------------------

- (void)rotateLeft:(id)sender {
    (void)sender;
    _rotationTurns = (_rotationTurns + 3) % 4;
    [self finishingChanged:nil];
    [self updateApOverlay];
}

- (void)rotateRight:(id)sender {
    (void)sender;
    _rotationTurns = (_rotationTurns + 1) % 4;
    [self finishingChanged:nil];
    [self updateApOverlay];
}

- (void)autoCrop:(id)sender {
    (void)sender;
    if (!_stacked) return;
    int x = 0, y = 0, w = 0, h = 0;
    const int margin = std::max(0, std::min(4096, [_cropMarginField intValue]));
    stackcore::detect_object_bounds(*_stacked, margin, x, y, w, h);
    _cropRect = NSMakeRect(x, y, w, h);
    [_cropCheck setState:NSControlStateValueOn];
    [self finishingChanged:nil];
    [self updateApOverlay];
}

- (void)clearCrop:(id)sender {
    (void)sender;
    _cropRect = NSZeroRect;
    [_cropCheck setState:NSControlStateValueOff];
    [_cropLabel setStringValue:LSLocalizedString(@"切り抜く範囲はまだありません")];
    [self finishingChanged:nil];
    [self updateApOverlay];
}

// ---- 書き出し -------------------------------------------------------------

- (OutputFormat)currentOutputFormat {
    const NSInteger index = [_formatPopup indexOfSelectedItem];
    if (index == 1) return OutputFormat::TiffFloat32;
    if (index == 2) return OutputFormat::FitsFloat32;
    if (index == 3) return OutputFormat::Png16;
    return OutputFormat::Tiff16;
}

- (NSString*)outputExtension {
    switch ([self currentOutputFormat]) {
        case OutputFormat::Png16: return @"png";
        case OutputFormat::FitsFloat32: return @"fits";
        default: return @"tif";
    }
}

// スタックした画像の撮影時刻の中央（SERのタイムスタンプ）。無ければ NO。
- (BOOL)stackedMidTicks:(std::int64_t*)ticks {
    if (!_previewSource || _stackedFrames.empty()) return NO;
    return stackcore::mid_timestamp(*_previewSource, _stackedFrames, *ticks) ? YES : NO;
}

// ファイル名。**スタックしたときの条件**で付ける（B3）。つまみを後から動かしても変わらない。
//   標準     : {元名}_ap{AP}_{率}_x{倍率}.拡張子 / 画像全体のみなら {元名}_global_{率}
//   WinJUPOS : {撮影時刻}-{対象名}.拡張子（撮影時刻が分からなければ標準にする）
- (NSString*)outputNameForPercentText:(NSString*)selection {
    NSString* base = _inputIsSequence && !_sequenceFiles.empty()
                         ? [[[NSString stringWithUTF8String:_sequenceFiles.front().c_str()] lastPathComponent]
                               stringByDeletingPathExtension]
                         : [[[self inputPathString] lastPathComponent] stringByDeletingPathExtension];
    NSString* object = [[_objectField stringValue]
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    object = [[object componentsSeparatedByCharactersInSet:
                          [NSCharacterSet characterSetWithCharactersInString:@"/:"]] componentsJoinedByString:@"-"];
    if ([_nameStylePopup indexOfSelectedItem] == 1) {
        std::int64_t ticks = 0;
        if ([self stackedMidTicks:&ticks]) {
            NSString* time = [NSString stringWithUTF8String:stackcore::ticks_to_winjupos(ticks).c_str()];
            NSString* who = [object length] > 0 ? object : base;
            return [NSString stringWithFormat:@"%@-%@.%@", time, who, [self outputExtension]];
        }
    }
    NSDictionary* info = _stackedInfo;
    const double scale = info ? [info[@"drizzle"] doubleValue]
                              : LSDrizzleScaleAt([_drizzleSegment selectedSegment]);
    NSString* zoom = (scale == 1.0) ? @""
                                    : [NSString stringWithFormat:@"_x%@",
                                                                 scale == 1.5 ? @"1.5"
                                                                              : [NSString stringWithFormat:@"%.0f", scale]];
    const BOOL globalOnly = info ? [info[@"globalOnly"] boolValue] : [_methodPopup indexOfSelectedItem] == 1;
    NSString* prefix = [object length] > 0 ? [NSString stringWithFormat:@"%@_%@", base, object] : base;
    if (globalOnly) {
        return [NSString stringWithFormat:@"%@_global_%@%@.%@", prefix, selection, zoom, [self outputExtension]];
    }
    int apSize = info ? [info[@"apSize"] intValue] : (_analysis ? _analysis->ap_size : LSApSizeAt([_apSizePopup indexOfSelectedItem]));
    if (apSize == 0) apSize = 64;
    return [NSString stringWithFormat:@"%@_ap%d_%@%@.%@", prefix, apSize, selection, zoom,
                                      [self outputExtension]];
}

- (NSString*)defaultOutputName {
    NSString* selection = _stackedInfo[@"selection"];
    if (!selection) {
        if ([_methodPopup indexOfSelectedItem] == 1) {
            selection = [NSString stringWithFormat:@"%.0fpct", [_topSlider doubleValue]];
        } else {
            selection = _selectionUsesCount ? [NSString stringWithFormat:@"%dframes", _apTopCountSetting]
                                            : [NSString stringWithFormat:@"%.0fpct", _apTopPercentSetting];
        }
    }
    return [self outputNameForPercentText:selection];
}

- (void)updateNamePreview {
    if (_inputPath.empty()) {
        [_namePreview setStringValue:@""];
        return;
    }
    [_namePreview setStringValue:[self defaultOutputName]];
}

- (void)formatChanged:(id)sender {
    (void)sender;
    [self updateNamePreview];
    if (!_restoringSettings) [self persistSettings];
}

// 書き出しに添える情報（F6）。現在時刻は入れない（決定論性、仕様書 §7.1）。
- (stackcore::ImageMetadata)metadataForExport {
    stackcore::ImageMetadata m;
    if ([_metadataCheck state] != NSControlStateValueOn) return m;
    NSString* version = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    m.software = std::string("LunaStack ") + (version ? [version UTF8String] : "");
    NSString* object = [[_objectField stringValue]
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    m.object = [object UTF8String];
    std::int64_t ticks = 0;
    if ([self stackedMidTicks:&ticks]) m.date_obs = stackcore::ticks_to_iso8601(ticks);
    NSDictionary* info = _stackedInfo;
    m.frames_combined = [info[@"framesCombined"] intValue];
    NSString* summary = [NSString stringWithFormat:@"input %@; %@ stack %@; drizzle %.1f",
                                                   [self inputDisplayName],
                                                   [info[@"globalOnly"] boolValue] ? @"global" : @"MAP",
                                                   info[@"selection"] ? info[@"selection"] : @"",
                                                   [info[@"drizzle"] doubleValue]];
    m.description = [summary UTF8String];
    const stackcore::FinishingSettings f = [self currentFinishingSettings];
    char buf[256];
    std::string sharpen = "wavelet sharpen";
    for (const auto& p : f.wavelet) {
        std::snprintf(buf, sizeof(buf), " %.2f", p.sharpen);
        sharpen += buf;
    }
    sharpen += " denoise";
    for (const auto& p : f.wavelet) {
        std::snprintf(buf, sizeof(buf), " %.2f", p.denoise);
        sharpen += buf;
    }
    m.history.push_back(sharpen);
    if (f.channels.any()) {
        std::snprintf(buf, sizeof(buf), "channel align R %.2f,%.2f B %.2f,%.2f", f.channels.red_dx,
                      f.channels.red_dy, f.channels.blue_dx, f.channels.blue_dy);
        m.history.push_back(buf);
    }
    if (!f.color.identity()) {
        std::snprintf(buf, sizeof(buf), "color gain %.3f %.3f %.3f saturation %.2f", f.color.gain[0],
                      f.color.gain[1], f.color.gain[2], f.color.saturation);
        m.history.push_back(buf);
    }
    if (f.stretch) {
        std::snprintf(buf, sizeof(buf), "stretch black %.3f white %.3f gamma %.2f", f.black, f.white, f.gamma);
        m.history.push_back(buf);
    }
    if (f.dering > 0.0) {
        std::snprintf(buf, sizeof(buf), "dering %.2f", f.dering);
        m.history.push_back(buf);
    }
    if (_calibration && !_calibration->empty()) m.history.push_back("calibrated (dark/flat)");
    return m;
}

- (void)save:(id)sender {
    (void)sender;
    if (!_stacked || _running) return;

    NSSavePanel* panel = [NSSavePanel savePanel];
    const OutputFormat format = [self currentOutputFormat];
    if (format == OutputFormat::Png16) [panel setAllowedFileTypes:@[ @"png" ]];
    else if (format == OutputFormat::FitsFloat32) [panel setAllowedFileTypes:@[ @"fits", @"fit" ]];
    else [panel setAllowedFileTypes:@[ @"tif", @"tiff" ]];
    [panel setNameFieldStringValue:[self defaultOutputName]];
    // 保存先は前回書き出したフォルダ、無ければ入力と同じフォルダ。
    NSString* last = [[NSUserDefaults standardUserDefaults] stringForKey:@"lastExportDirectory"];
    NSString* dir = ([last length] > 0 && [[NSFileManager defaultManager] fileExistsAtPath:last])
                        ? last
                        : (_inputIsSequence ? [self inputPathString]
                                            : [[self inputPathString] stringByDeletingLastPathComponent]);
    [panel setDirectoryURL:[NSURL fileURLWithPath:dir]];
    if ([panel runModal] != NSModalResponseOK) return;
    NSURL* url = [panel URL];
    if (!url) return;
    [[NSUserDefaults standardUserDefaults] setObject:[[url path] stringByDeletingLastPathComponent]
                                              forKey:@"lastExportDirectory"];

    try {
        // 書き出しは画面の表示と同じ処理系で、本解像度で描き直してから行う。
        const std::shared_ptr<stackcore::FrameBuffer> image = [self renderFinishingNow];
        write_output_image(std::string([[url path] UTF8String]), *image, format, [self metadataForExport]);
        [_statusLabel setStringValue:[NSString stringWithFormat:LSLocalizedString(@"保存しました: %@"),
                                                                [[url path] lastPathComponent]]];
    } catch (const std::exception& e) {
        [self showError:[NSString stringWithUTF8String:e.what()]
                  title:LSLocalizedString(@"保存できませんでした")];
    }
}

// 採用率だけを変えた結果をまとめて書き出す（F4）。解析はやり直さない。
- (void)exportMultiplePercents:(id)sender {
    (void)sender;
    if (_running || !_stacked || ![self alignmentUsable]) return;
    NSMutableArray* values = [NSMutableArray array];
    for (NSString* part in [[_multiPercentField stringValue] componentsSeparatedByString:@","]) {
        NSString* t = [part stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if ([t length] == 0) continue;
        const double v = [t doubleValue];
        if (v > 0.0) [values addObject:@(v)];
    }
    if ([values count] == 0) {
        [self showError:LSLocalizedString(@"採用率をカンマ区切りで入力してください（例: 5, 10, 25）")
                  title:LSLocalizedString(@"まとめて書き出し")];
        return;
    }
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setCanChooseDirectories:YES];
    [panel setCanChooseFiles:NO];
    [panel setCanCreateDirectories:YES];
    [panel setPrompt:LSLocalizedString(@"ここに書き出す")];
    [panel setMessage:LSLocalizedString(@"書き出すフォルダを選んでください（同名のファイルは上書きせず番号を付けます）")];
    if ([panel runModal] != NSModalResponseOK) return;
    NSString* folder = [[[panel URLs] firstObject] path];

    const BOOL globalOnly = [_methodPopup indexOfSelectedItem] == 1;
    const BOOL counts = !globalOnly && _selectionUsesCount;
    JobRequest base;
    base.path = _inputPath;
    base.options = [self currentOpenOptions];
    base.settings = [self currentSettings];
    // 倍率は表示中の結果を作ったときのものに揃える。名前・切り抜き・チャンネルのずれは
    // その画像の座標で決めてあるので、つまみの今の値で作ると食い違う。
    if (_stackedInfo[@"drizzle"]) base.settings.drizzle_scale = [_stackedInfo[@"drizzle"] doubleValue];
    if (_stackedInfo[@"pixfrac"]) base.settings.pixfrac = [_stackedInfo[@"pixfrac"] doubleValue];
    base.global_only = globalOnly;
    base.low_memory = [_lowMemoryCheck state] == NSControlStateValueOn;
    base.stage = JobStage::Stack;
    base.global = _globalStage;
    base.analysis = _analysis;
    const stackcore::FinishingSettings finishing = [self currentFinishingSettings];
    const OutputFormat format = [self currentOutputFormat];
    const stackcore::ImageMetadata metadata = [self metadataForExport];
    NSMutableArray* names = [NSMutableArray array];
    for (NSNumber* v in values) {
        NSString* selection = counts ? [NSString stringWithFormat:@"%dframes", [v intValue]]
                                     : [NSString stringWithFormat:@"%gpct", [v doubleValue]];
        NSString* name = [self outputNameForPercentText:selection];
        // 既存のファイルは上書きしない（B6の教訓）。
        NSString* path = [folder stringByAppendingPathComponent:name];
        int n = 2;
        while ([[NSFileManager defaultManager] fileExistsAtPath:path]) {
            path = [folder stringByAppendingPathComponent:
                               [NSString stringWithFormat:@"%@_%d.%@", [name stringByDeletingPathExtension], n++,
                                                          [name pathExtension]]];
        }
        [names addObject:path];
    }

    _running = YES;
    _cancelFlag->store(false);
    [self resetEta];
    [_progress setHidden:NO];
    [self updateControlsEnabled];
    std::atomic<bool>* cancelFlag = _cancelFlag;
    MainWindowController* controller = self;
    NSArray* frozenValues = [values copy];
    NSArray* frozenNames = [names copy];

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        int written = 0;
        std::string error;
        bool cancelled = false;
        for (NSUInteger i = 0; i < [frozenValues count]; ++i) {
            if (cancelFlag->load()) {
                cancelled = true;
                break;
            }
            JobRequest req = base;
            const double v = [frozenValues[i] doubleValue];
            if (globalOnly) req.settings.reference_top_percent = std::min(100.0, v);
            else if (counts) req.settings.ap_top_count = std::max(1, static_cast<int>(v));
            else {
                req.settings.ap_top_percent = std::min(100.0, v);
                req.settings.ap_top_count = 0;
            }
            NSString* label = [NSString stringWithFormat:LSLocalizedString(@"まとめて書き出し %lu/%lu"),
                                                         (unsigned long)(i + 1),
                                                         (unsigned long)[frozenValues count]];
            const stackcore::ProgressFn progress = [controller, cancelFlag, label](const char*, int done, int total) -> bool {
                if (cancelFlag->load()) return false;
                const int step = total < 100 ? 1 : total / 100;
                if (done % step == 0 || done == total) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        [controller reportStage:label done:done total:total];
                    });
                }
                return true;
            };
            JobResult result = run_job(req, progress);
            if (result.cancelled) {
                cancelled = true;
                break;
            }
            if (!result.error.empty() || !result.image) {
                error = result.error;
                break;
            }
            try {
                stackcore::FinishingPipeline pipeline;
                pipeline.set_input(result.image, kWaveletLayers);
                stackcore::FrameBuffer out;
                pipeline.render(finishing, out);
                stackcore::ImageMetadata m = metadata;
                if (!m.empty()) m.frames_combined = result.frames_combined;
                write_output_image(std::string([frozenNames[i] UTF8String]), out, format, m);
                ++written;
            } catch (const std::exception& e) {
                error = e.what();
                break;
            }
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            [controller finishMultiExportWritten:written
                                           total:static_cast<int>([frozenValues count])
                                          folder:folder
                                           error:error
                                       cancelled:cancelled];
            [frozenValues release];
            [frozenNames release];
        });
    });
}

- (void)finishMultiExportWritten:(int)written
                           total:(int)total
                          folder:(NSString*)folder
                           error:(const std::string&)error
                       cancelled:(bool)cancelled {
    _running = NO;
    [_progress setHidden:YES];
    [self resetEta];
    [self updateControlsEnabled];
    NSString* text = [NSString stringWithFormat:LSLocalizedString(@"まとめて書き出し: %d / %d 件を %@ に保存しました"),
                                                written, total, [folder lastPathComponent]];
    if (cancelled) text = [text stringByAppendingString:LSLocalizedString(@"（中断）")];
    [_statusLabel setStringValue:text];
    if (!error.empty()) {
        [self showError:[NSString stringWithUTF8String:error.c_str()]
                  title:LSLocalizedString(@"まとめて書き出しを完了できませんでした")];
    } else if (!cancelled) {
        [self notifyDone:text];
    }
    [self jobDidStop];
}

@end
