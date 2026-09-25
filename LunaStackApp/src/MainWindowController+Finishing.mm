#import "MainWindowController_Private.h"

#include <algorithm>
#include <cmath>

#include "stackcore/map_pipeline.hpp"
#include "stackcore/video_source.hpp"
#include "stackcore/wavelet.hpp"

@implementation MainWindowController (Finishing)

// ---- 後処理 ---------------------------------------------------------------

- (void)stretchToggled:(id)sender {
    [_preview setDisplayStretch:[(NSButton*)sender state] == NSControlStateValueOn];
}

- (void)waveletChanged:(id)sender {
    (void)sender;
    for (int j = 0; j < kWaveletLayers; ++j) {
        [_sharpenValues[j] setStringValue:[NSString stringWithFormat:@"%.2f",
                                                                    [_sharpenSliders[j]
                                                                        doubleValue]]];
        [_denoiseValues[j] setStringValue:[NSString stringWithFormat:@"%.2f",
                                                                    [_denoiseSliders[j]
                                                                        doubleValue]]];
    }
    [self applyWavelet];
}

- (void)waveletPreviewChanged:(id)sender {
    (void)sender;
    if (!_stacked || !_displayed) return;
    [_viewModeSegment setSelectedSegment:2];
    const BOOL showEffect = [_waveletPreviewCheck state] == NSControlStateValueOn;
    [_preview showFrameBuffer:showEffect ? *_displayed : *_stacked];
    [_statusLabel
        setStringValue:LSLocalizedString(showEffect ? @"ウェーブレット効果ありのプレビュー"
                                              : @"ウェーブレット効果なしのプレビュー")];
}

- (void)resetWavelet:(id)sender {
    (void)sender;
    for (int j = 0; j < kWaveletLayers; ++j) {
        [_sharpenSliders[j] setDoubleValue:1.0];
        [_denoiseSliders[j] setDoubleValue:0.0];
    }
    [self waveletChanged:nil];
}

- (void)applyWavelet {
    if (!_wavelet || !_wavelet->ready()) return;

    std::vector<stackcore::WaveletLayerParams> params(kWaveletLayers);
    for (int j = 0; j < kWaveletLayers; ++j) {
        params[static_cast<std::size_t>(j)].sharpen = [_sharpenSliders[j] doubleValue];
        params[static_cast<std::size_t>(j)].denoise = [_denoiseSliders[j] doubleValue];
    }

    auto out = std::make_shared<stackcore::FrameBuffer>();
    _wavelet->synthesize(params, *out);
    out->set_source_bit_depth(_stacked->source_bit_depth());
    _displayed = out;
    const BOOL showEffect = [_waveletPreviewCheck state] == NSControlStateValueOn;
    [_preview showFrameBuffer:showEffect ? *_displayed : *_stacked];
}

- (void)setDrizzleIndexForTesting:(int)index {
    if (index < 0 || index > 3) return;
    [_drizzleSegment setSelectedSegment:index];
    [self drizzleChanged:nil];
}

- (void)setZoomIndexForTesting:(int)index {
    if (index < 0 || index > 3) return;
    [_zoomControl setSelectedSegment:index];
    [self zoomChanged:nil];
}

- (void)setApHeatmapForTesting:(BOOL)on {
    [_apHeatCheck setState:on ? NSControlStateValueOn : NSControlStateValueOff];
    [self apDisplayChanged:nil];
}

- (void)setSharpenForTesting:(double)value denoise:(double)denoise {
    for (int j = 0; j < kWaveletLayers; ++j) {
        [_sharpenSliders[j] setDoubleValue:(j < 3 ? value : 1.0)];
        [_denoiseSliders[j] setDoubleValue:denoise];
    }
    [self waveletChanged:nil];
}

- (void)setWaveletPreviewForTesting:(BOOL)on {
    [_waveletPreviewCheck setState:on ? NSControlStateValueOn : NSControlStateValueOff];
    [self waveletPreviewChanged:nil];
}

// ---- 書き出し -------------------------------------------------------------

- (OutputFormat)currentOutputFormat {
    const NSInteger index = [_formatPopup indexOfSelectedItem];
    if (index == 1) return OutputFormat::TiffFloat32;
    if (index == 2) return OutputFormat::FitsFloat32;
    if (index == 3) return OutputFormat::Png16;
    return OutputFormat::Tiff16;
}

// UI設計書 §4.6 の命名規則 `{元名}_ap{AP}_{率}pct_x{倍率}.拡張子`。
- (NSString*)outputNameForPath:(NSString*)path apSize:(int)apSize {
    const double scale = LSDrizzleScaleAt([_drizzleSegment selectedSegment]);
    NSString* base = [[path lastPathComponent] stringByDeletingPathExtension];
    NSString* zoom = (scale == 1.0)
                         ? @""
                         : [NSString stringWithFormat:@"_x%@",
                                                      (scale == 1.5) ? @"1.5"
                                                                     : [NSString stringWithFormat:
                                                                                     @"%.0f", scale]];
    NSString* extension = @"tif";
    if ([self currentOutputFormat] == OutputFormat::Png16) extension = @"png";
    else if ([self currentOutputFormat] == OutputFormat::FitsFloat32) extension = @"fits";
    NSString* selection = _selectionUsesCount
                              ? [NSString stringWithFormat:@"%dframes", _apTopCountSetting]
                              : [NSString stringWithFormat:@"%.0fpct", _apTopPercentSetting];
    return [NSString stringWithFormat:@"%@_ap%d_%@%@.%@", base, apSize, selection, zoom,
                                      extension];
}

- (void)formatChanged:(id)sender {
    (void)sender;
    [self updateNamePreview];
}

- (void)updateNamePreview {
    if (_inputPath.empty()) {
        [_namePreview setStringValue:@""];
        return;
    }
    int ap = _analysis ? _analysis->ap_size : LSApSizeAt([_apSizePopup indexOfSelectedItem]);
    if (ap == 0) ap = 64;
    [_namePreview setStringValue:[self outputNameForPath:[self inputPathString]
                                                  apSize:ap]];
}

- (void)save:(id)sender {
    (void)sender;
    if (!_displayed) return;

    NSSavePanel* panel = [NSSavePanel savePanel];
    const OutputFormat format = [self currentOutputFormat];
    if (format == OutputFormat::Png16) [panel setAllowedFileTypes:@[ @"png" ]];
    else if (format == OutputFormat::FitsFloat32) [panel setAllowedFileTypes:@[ @"fits", @"fit" ]];
    else [panel setAllowedFileTypes:@[ @"tif" ]];
    [panel setNameFieldStringValue:[_namePreview stringValue]];
    if ([panel runModal] != NSModalResponseOK) return;
    NSURL* url = [panel URL];
    if (!url) return;

    try {
        write_output_image(std::string([[url path] UTF8String]), *_displayed, format);
        [_statusLabel setStringValue:[NSString stringWithFormat:LSLocalizedString(@"保存しました: %@"),
                                                                [[url path] lastPathComponent]]];
    } catch (const std::exception& e) {
        [self showError:[NSString stringWithUTF8String:e.what()]
                  title:LSLocalizedString(@"保存できませんでした")];
    }
}

@end
