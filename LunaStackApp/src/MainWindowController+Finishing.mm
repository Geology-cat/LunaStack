#import "MainWindowController_Private.h"

#include <algorithm>
#include <cmath>

#include "stackcore/metadata.hpp"

namespace {

// 仕上げを掛けない（変化なし）ときの既定の配分。連動で強さを上げたときの形。
const double kLinkedProfile[kWaveletLayers] = {7.0, 3.0, 1.0, 0.0, 0.0, 0.0};

double ParseField(NSTextField* field) {
    NSScanner* scanner = [NSScanner scannerWithString:[field stringValue]];
    double v = 0.0;
    if (![scanner scanDouble:&v] || !std::isfinite(v)) return 0.0;
    return std::max(-64.0, std::min(64.0, v));
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
    s.levels.master = _levels[0];
    for (int c = 0; c < 3; ++c) s.levels.channel[c] = _levels[c + 1];
    s.geometry.rotate_quarter_turns = _rotationTurns;
    s.geometry.flip_horizontal = [_flipHCheck state] == NSControlStateValueOn;
    s.geometry.flip_vertical = [_flipVCheck state] == NSControlStateValueOn;
    // 切り抜きは［切り抜く］でスタック結果そのものに済ませるので、ここでは掛けない。
    return s;
}

// 画面に出す仕上げ。「ウェーブレットの効果をプレビュー」がOFFなら、ウェーブレットと
// 輪抑制だけを外して描く（書き出しは常に currentFinishingSettings を使う）。
- (stackcore::FinishingSettings)previewFinishingSettings {
    stackcore::FinishingSettings s = [self currentFinishingSettings];
    if ([_waveletOnlyPreviewCheck state] != NSControlStateValueOn) {
        s.wavelet.assign(kWaveletLayers, stackcore::WaveletLayerParams());
        s.dering = 0.0;
    }
    return s;
}

- (void)waveletOnlyPreviewChanged:(id)sender {
    (void)sender;
    if (!_stacked) return;
    const BOOL on = [_waveletOnlyPreviewCheck state] == NSControlStateValueOn;
    // 仕上げ全体をOFFにしていると違いが見えないので、比べるときは全体をONにする。
    if ([_waveletPreviewCheck state] != NSControlStateValueOn) {
        [_waveletPreviewCheck setState:NSControlStateValueOn];
    }
    [_viewModeSegment setSelectedSegment:2];
    _displayed.reset();
    [self requestFinishingRender];
    [self updateFrameInfoLabel];
    [_statusLabel setStringValue:LSLocalizedString(on ? @"ウェーブレットの効果ありのプレビュー"
                                                     : @"ウェーブレットの効果なしのプレビュー（ほかの仕上げは掛けたまま）")];
}

// 新しいスタックを始める・入力を変えるときに、前の結果と描画待ちを捨てる。
- (void)resetFinishingForNewStack {
    ++_renderGeneration;
    _renderPending = NO;
    _stackedDisplayLow = 0.0f;
    _stackedDisplayHigh = 0.0f;
    _stacked.reset();
    _displayed.reset();
    _finishing.reset();
    _renderTargets[0].reset();
    _renderTargets[1].reset();
    [_stackedInfo release];
    _stackedInfo = nil;
    _stackedFrames.clear();
    _cropRect = NSZeroRect;
    _uncroppedStacked.reset();
    _appliedCrop = NSZeroRect;
    [_cropModeCheck setState:NSControlStateValueOff];
    [_preview setCropEditing:NO];
    [_preview setCropOverlay:NSZeroRect];
    [self updateCropControls];
}

// ---- 描画 -----------------------------------------------------------------

- (void)applyWavelet {
    [self requestFinishingRender];
}

// 仕上げ済みの画像を作る。重い処理なので直列キューで行う。
//
// **描画中に来た要求は捨てずに1つだけ控えておき、描き終わったらすぐ最新の設定で
// 次を描く。** 以前は要求のたびに番号を進め、描き上がった時点で番号が古ければ
// 結果を捨てていた。スライダーのドラッグでは描画より速く要求が来るので、
// ドラッグ中の結果がすべて捨てられて画面が止まり、指を止めた瞬間に飛んでいた。
// いまは「描いている間に動いた分」を次の1枚でまとめて描くので、描画の速さなりに
// 途切れず追従する。
//
// **常に本解像度で描く。** 以前はドラッグ中だけ半分の解像度の下書きを出していたが、
// 半分の解像度では最も細かいレイヤー（レイヤー1）の効果が表せず、拡大し直すと
// ぼやけるので、動かした瞬間に「ウェーブレットを掛けていない画像」が一瞬見えていた。
// 画面は「動かす前の仕上げ → 動かした後の仕上げ」とだけ移り変わる。
- (void)requestFinishingRender {
    if (!_finishing || !_stacked) return;
    if (_renderInFlight) {
        _renderPending = YES;
        return;
    }
    [self startFinishingRender];
}

- (void)startFinishingRender {
    const stackcore::FinishingSettings settings = [self previewFinishingSettings];
    const long generation = _renderGeneration;
    std::shared_ptr<stackcore::FinishingPipeline> pipeline = _finishing;
    // 出力先は使い回す（大きな画像で毎回確保すると、それだけで遅くなる）。
    // 画面や書き出しがまだ持っている画像には書き込まない。
    std::shared_ptr<stackcore::FrameBuffer> out;
    for (int i = 0; i < 2; ++i) {
        if (_renderTargets[i] && _renderTargets[i].use_count() == 1) {
            out = _renderTargets[i];
            break;
        }
    }
    if (!out) {
        out = std::make_shared<stackcore::FrameBuffer>();
        const int slot = _renderTargets[0] ? (_renderTargets[1] ? (_nextRenderSlot++ & 1) : 1) : 0;
        _renderTargets[slot] = out;
    }

    // 画面用の8bit画像も描画と同じ裏のスレッドで作る（大きな画像でメインが詰まらないように）。
    // 明るさの対応は表示するときと同じものを使う。違っていれば表示の側で作り直す。
    // ここでは表示を変えない（描き上がる前に今の画像の明るさだけ変わると、一瞬ちらつく）。
    // 表示に使う対応を計算だけしておき、結果を出すときにプレビューへ設定する。
    const LSDisplayMapping mapping = [self finishingDisplayMapping];
    // ヒストグラムは明るさの欄が見えているときだけ数える（大きな画像では1回ぶんの手間が大きい）。
    const bool wantHistogram = [self levelsHistogramVisible] ? true : false;
    if (!wantHistogram) _levelsHistogramStale = YES;
    const bool prepare = [_viewModeSegment selectedSegment] == 2 &&
                         [_waveletPreviewCheck state] == NSControlStateValueOn;
    const double deviceScale = [_preview effectiveDeviceZoom];

    _renderInFlight = YES;
    _renderPending = NO;
    MainWindowController* controller = self;
    dispatch_async(_finishQueue, ^{
        std::string error;
        std::shared_ptr<LSPreviewImage> prepared;
        auto histogram = std::make_shared<stackcore::LevelsHistogram>();
        try {
            pipeline->render(settings, *out, wantHistogram ? histogram.get() : nullptr);
            if (prepare) prepared = LSMakePreviewImage(*out, mapping, deviceScale);
        } catch (const std::exception& e) {
            error = e.what();
        }
        // メインのランループへ直接渡す（共通モード）。スライダーのドラッグ追跡中や、
        // 入れ子で回っているランループの中でも、描き上がった結果がすぐ届く。
        CFRunLoopRef mainLoop = CFRunLoopGetMain();
        CFRunLoopPerformBlock(mainLoop, kCFRunLoopCommonModes, ^{
            [controller finishedRender:out prepared:prepared histogram:histogram generation:generation error:error];
        });
        CFRunLoopWakeUp(mainLoop);
    });
}

- (void)finishedRender:(std::shared_ptr<stackcore::FrameBuffer>)out
              prepared:(std::shared_ptr<LSPreviewImage>)prepared
             histogram:(std::shared_ptr<stackcore::LevelsHistogram>)histogram
            generation:(long)generation
                 error:(const std::string&)error {
    _renderInFlight = NO;
    // 結果を捨てるのは、入力そのものが変わったとき（新しいスタック・クリア）だけ。
    const BOOL current = generation == _renderGeneration;
    if (current && !error.empty()) {
        [_statusLabel setStringValue:[NSString stringWithUTF8String:error.c_str()]];
    } else if (current) {
        // 描き上がった最新の仕上げ。表示の切り替えなどで描き直すときも、スタックそのままの
        // 画像へ戻さずこれを出す。
        _displayed = out;
        if (histogram && !histogram->empty()) {
            _levelsHistogram = *histogram;
            _levelsHistogramStale = NO;
            [self updateLevelsControls];
        }
        if ([_viewModeSegment selectedSegment] == 2 &&
            [_waveletPreviewCheck state] == NSControlStateValueOn) {
            ++_previewUpdates;
            [_preview showSharedFrame:out prepared:prepared mapping:[self finishingDisplayMapping]];
            [self updateApOverlay];
            if (_previewUpdateHook) _previewUpdateHook(*out);
        }
    }
    if (!_finishing || !_stacked) return;
    // 描いている間にまた動いた。最新の設定ですぐ次を描く。
    if (_renderPending) [self startFinishingRender];
}

// 仕上げの表示では、明るさの基準をスタック結果に固定する（強調するたびに明るさが揺れない）。
// 黒点・白点を自分で調整しているときは、その結果をそのまま（線形に）見せる。
- (void)applyFinishingDisplayRange {
    if (!_stacked) return;
    const LSDisplayMapping m = [self finishingDisplayMapping];
    if (m.stretch) [_preview setFixedStretchLow:m.lo high:m.hi gamma:m.gamma];
}

// 仕上げの表示に使う明るさの対応（プレビューには触らない）。
- (LSDisplayMapping)finishingDisplayMapping {
    LSDisplayMapping m;
    if (!_stacked || ![_preview displayStretch]) return m;  // 「表示を明るくする」OFFは素の値
    m.stretch = true;
    if (!(_stackedDisplayHigh > _stackedDisplayLow)) {
        float lo = 1.0f, hi = 0.0f;
        for (int c = 0; c < _stacked->channels(); ++c) {
            for (int y = 0; y < _stacked->height(); ++y) {
                const float* r = _stacked->row(c, y);
                for (int x = 0; x < _stacked->width(); ++x) {
                    lo = std::min(lo, r[x]);
                    hi = std::max(hi, r[x]);
                }
            }
        }
        _stackedDisplayLow = lo;
        _stackedDisplayHigh = hi > lo ? hi : lo + 1e-6f;
    }
    // 「表示を明るくする」: スタック結果の明るさの範囲を、RGB 全体のレベル補正に通した範囲が
    // 画面いっぱいになるよう、画面だけ直線で引き伸ばす（ガンマは掛けない）。
    //   * 初期値のままでも暗い惑星が見える
    //   * 三角を少し動かしても見た目が連続して変わる（動かした瞬間に暗く飛ばない）
    //   * 黒・白の三角をデータの端（またはその内側）に合わせると範囲が 0..1 になり、
    //     画面は書き出す明るさとちょうど同じになる
    const stackcore::Levels& l = _levels[0];
    const auto level = [&l](double v) {
        double t = (v - l.black) / std::max(1e-9, l.white - l.black);
        t = std::min(1.0, std::max(0.0, t));
        return l.gamma == 1.0 ? t : std::pow(t, 1.0 / l.gamma);
    };
    double lo = level(_stackedDisplayLow), hi = level(_stackedDisplayHigh);
    if (!(hi - lo > 1e-6)) {
        lo = 0.0;
        hi = 1.0;
    }
    m.lo = static_cast<float>(lo);
    m.hi = static_cast<float>(hi);
    m.gamma = 1.0f;
    return m;
}

// 仕上げを同期して描く（書き出しと自己検証用）。
- (std::shared_ptr<stackcore::FrameBuffer>)renderFinishingNow {
    return [self renderFinishingNowWithSettings:[self currentFinishingSettings]];
}

- (std::shared_ptr<stackcore::FrameBuffer>)renderFinishingNowWithSettings:
    (const stackcore::FinishingSettings&)settings {
    if (!_finishing) return nullptr;
    std::shared_ptr<stackcore::FinishingPipeline> pipeline = _finishing;
    auto out = std::make_shared<stackcore::FrameBuffer>();
    __block std::string error;
    stackcore::FrameBuffer* target = out.get();
    auto histogram = std::make_shared<stackcore::LevelsHistogram>();
    stackcore::LevelsHistogram* hist = histogram.get();
    dispatch_sync(_finishQueue, ^{
        try {
            pipeline->render(settings, *target, hist);
        } catch (const std::exception& e) {
            error = e.what();
        }
    });
    if (!error.empty()) throw std::runtime_error(error);
    if (!histogram->empty()) {
        _levelsHistogram = *histogram;
        [self updateLevelsControls];
    }
    return out;
}

- (void)showFinishedOrStacked {
    if (!_stacked) return;
    const BOOL showEffect = [_waveletPreviewCheck state] == NSControlStateValueOn;
    [self applyFinishingDisplayRange];
    if (showEffect && _displayed) {
        [_preview showSharedFrame:_displayed];
    } else {
        [_preview showSharedFrame:_stacked];
        if (showEffect) [self requestFinishingRender];
    }
    [self updateApOverlay];
}

// ---- つまみの操作 -----------------------------------------------------------

- (void)updateFinishingValueLabels {
    // ±ボタン・数値欄つきのつまみ。入力中の数値欄は書き換えない（打っている途中で消えないように）。
    const BOOL linked = [_linkedCheck state] == NSControlStateValueOn;
    for (int i = 0; i < kAdjustCount; ++i) {
        if (!_adjSliders[i]) continue;
        if ([_adjFields[i] currentEditor] == nil) {
            [_adjFields[i] setStringValue:[NSString stringWithFormat:@"%.2f", [_adjSliders[i] doubleValue]]];
        }
        // 連動中は各レイヤーの強調を直接動かせない（強さで一括して動かす）。
        BOOL enabled = YES;
        if (i < kAdjustLinked && i % 2 == 0) enabled = !linked;
        if (i == kAdjustLinked) enabled = linked;
        for (NSControl* c in @[ _adjSliders[i], _adjFields[i], _adjMinus[i], _adjPlus[i] ]) [c setEnabled:enabled];
    }
    for (int c = 0; c < 3; ++c) {
        [_gainValues[c] setStringValue:[NSString stringWithFormat:@"%.3f", [_gainSliders[c] doubleValue]]];
    }
    [_saturationValue setStringValue:[NSString stringWithFormat:@"%.2f", [_saturationSlider doubleValue]]];
    [self updateLevelsControls];
    NSString* rotation = _rotationTurns == 0 ? LSLocalizedString(@"回転なし")
                                             : [NSString stringWithFormat:LSLocalizedString(@"右へ %d°"),
                                                                          _rotationTurns * 90];
    [_rotationLabel setStringValue:rotation];
    [self updateCropControls];
}

// ---- ±ボタン・数値欄・レイヤーの初期化 -----------------------------------------

// つまみ index の値を変えたあと、スライダーを動かしたのと同じ処理を通す。
- (void)adjustableChanged:(int)index {
    if (index == kAdjustDering) {
        [self finishingChanged:_adjSliders[index]];
    } else {
        [self waveletChanged:_adjSliders[index]];
    }
}

// 押し続けたときは、続けて押されている回数に応じて1回の変化を大きくする
// （はじめは細かく、長く押せば速く動く）。
- (void)stepAdjustable:(int)index direction:(int)direction {
    if (index < 0 || index >= kAdjustCount || !_adjSliders[index]) return;
    static NSTimeInterval lastTime = 0.0;
    static int lastIndex = -1;
    static int repeats = 0;
    const NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
    repeats = (index == lastIndex && now - lastTime < 0.2) ? repeats + 1 : 0;
    lastTime = now;
    lastIndex = index;
    const double factor = repeats > 30 ? 10.0 : (repeats > 10 ? 4.0 : 1.0);
    NSSlider* slider = _adjSliders[index];
    const double step = _adjSteps[index] * factor;
    // 刻みの格子に乗せる（0.05刻みなら 1.37 → 1.40 のように揃える）。
    double v = std::round([slider doubleValue] / _adjSteps[index]) * _adjSteps[index] + direction * step;
    v = std::max([slider minValue], std::min([slider maxValue], v));
    [slider setDoubleValue:v];
    [self adjustableChanged:index];
}

- (void)adjustMinus:(id)sender {
    [self stepAdjustable:static_cast<int>([sender tag]) direction:-1];
}

- (void)adjustPlus:(id)sender {
    [self stepAdjustable:static_cast<int>([sender tag]) direction:+1];
}

// 数値欄に直接入力した値を反映する。範囲外は収め、読めない値は元に戻す。
- (void)adjustFieldChanged:(id)sender {
    NSTextField* field = (NSTextField*)sender;
    const int index = static_cast<int>([field tag]);
    if (index < 0 || index >= kAdjustCount || !_adjSliders[index]) return;
    NSSlider* slider = _adjSliders[index];
    NSString* text = [[field stringValue] stringByTrimmingCharactersInSet:
                                              [NSCharacterSet whitespaceCharacterSet]];
    // 全角の数字・小数点も受け付ける。
    NSMutableString* ascii = [NSMutableString stringWithString:text];
    CFStringTransform((CFMutableStringRef)ascii, NULL, kCFStringTransformFullwidthHalfwidth, false);
    NSScanner* scanner = [NSScanner scannerWithString:ascii];
    double v = 0.0;
    if (![scanner scanDouble:&v] || ![scanner isAtEnd] || !std::isfinite(v)) {
        NSBeep();
        [field setStringValue:[NSString stringWithFormat:@"%.2f", [slider doubleValue]]];
        return;
    }
    v = std::max([slider minValue], std::min([slider maxValue], v));
    if (v == [slider doubleValue]) {
        [field setStringValue:[NSString stringWithFormat:@"%.2f", v]];
        return;
    }
    [slider setDoubleValue:v];
    [self adjustableChanged:index];
    [field setStringValue:[NSString stringWithFormat:@"%.2f", v]];
}

// レイヤー1つを初期値（強調1.00・ノイズ0.00）に戻す。
- (void)resetLayer:(id)sender {
    const int j = static_cast<int>([sender tag]);
    if (j < 0 || j >= kWaveletLayers) return;
    if ([_linkedCheck state] == NSControlStateValueOn) {
        // 連動中に1層だけ戻すと配分が崩れるので、連動を外してから戻す。
        [_linkedCheck setState:NSControlStateValueOff];
    }
    [_sharpenSliders[j] setDoubleValue:1.0];
    [_denoiseSliders[j] setDoubleValue:0.0];
    [self waveletChanged:nil];
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
    [self requestFinishingRender];
}

- (void)finishingChanged:(id)sender {
    if (sender == _flipHCheck || sender == _flipVCheck) [self invalidateCropBox];
    [self updateFinishingValueLabels];
    [self requestFinishingRender];
}

- (void)waveletPreviewChanged:(id)sender {
    (void)sender;
    if (!_stacked) return;
    [self invalidateCropBox];
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
    settings.levels = stackcore::LevelsSettings();
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

// ［自動］: RGB 全体の黒・白を、明るさの分布の端（2% と 99.95%）に合わせる。
// チャンネル別の設定と中間（ガンマ）はそのまま残す。
- (void)applyToneBlack:(double)black white:(double)white {
    black = std::max(0.0, std::min(1.0 - 2.0 / 255.0, black));
    white = std::max(black + 2.0 / 255.0, std::min(1.0, white));
    _levels[0].black = std::round(black * 255.0 * 10.0) / (255.0 * 10.0);
    _levels[0].white = std::round(white * 255.0 * 10.0) / (255.0 * 10.0);
    [_levelsChannelPopup selectItemAtIndex:0];
    [self finishingChanged:nil];
}

// ---- レベル補正 -------------------------------------------------------------------

// 明るさ（レベル補正）の欄が画面に出ているか（仕上げ・出力タブで、欄が開いている）。
- (BOOL)levelsHistogramVisible {
    return [_inspectorTab selectedSegment] == 3 && [self sectionOpen:@"tone"];
}

// 欄を開いた・タブを移ったときに、まだ数えていなければ描き直して数える。
- (void)refreshLevelsHistogramIfNeeded {
    if (_stacked && _finishing && (_levelsHistogramStale || _levelsHistogram.empty()) &&
        [self levelsHistogramVisible]) {
        [self requestFinishingRender];
    }
}

- (int)levelsChannel {
    const NSInteger i = [_levelsChannelPopup indexOfSelectedItem];
    return i >= 0 && i < 4 ? static_cast<int>(i) : 0;
}

// 選んでいるチャンネルの値を、三角・数値欄・ヒストグラムに出す。
- (void)updateLevelsControls {
    if (!_levelsView) return;
    // モノクロでは R・G・B を選べない。
    const BOOL color = !_stacked || _stacked->channels() == 3;
    for (int i = 1; i < 4; ++i) [[_levelsChannelPopup itemAtIndex:i] setEnabled:color];
    if (!color && [self levelsChannel] != 0) [_levelsChannelPopup selectItemAtIndex:0];
    [_levelsChannelPopup setAutoenablesItems:NO];
    const int ch = [self levelsChannel];
    const stackcore::Levels& l = _levels[ch];
    [_levelsView setBlack:l.black];
    [_levelsView setWhite:l.white];
    [_levelsView setGamma:l.gamma];
    const int index = _levelsHistogram.channels == 3 ? ch : 0;
    if (!_levelsHistogram.empty() && !_levelsHistogram.counts[index].empty()) {
        [_levelsView setHistogram:_levelsHistogram.counts[index]];
    }
    const auto format255 = [](double v) {
        const double x = std::round(v * 255.0 * 10.0) / 10.0;
        return std::fabs(x - std::round(x)) < 1e-9 ? [NSString stringWithFormat:@"%.0f", x]
                                                   : [NSString stringWithFormat:@"%.1f", x];
    };
    // 打っている途中の欄は書き換えない。
    if ([_levelsBlackField currentEditor] == nil) [_levelsBlackField setStringValue:format255(l.black)];
    if ([_levelsWhiteField currentEditor] == nil) [_levelsWhiteField setStringValue:format255(l.white)];
    if ([_levelsGammaField currentEditor] == nil) {
        [_levelsGammaField setStringValue:[NSString stringWithFormat:@"%.2f", l.gamma]];
    }
}

- (void)levelsViewDidChange:(LevelsView*)view {
    stackcore::Levels& l = _levels[[self levelsChannel]];
    l.black = [view black];
    l.white = [view white];
    l.gamma = [view gamma];
    [self finishingChanged:view];
}

- (void)levelsChannelChanged:(id)sender {
    (void)sender;
    [self updateLevelsControls];
}

// 数値欄: 黒・白は 0〜255、中間は 0.10〜9.99。全角の数字も読み、範囲に収める。
- (void)levelsFieldChanged:(id)sender {
    NSTextField* field = (NSTextField*)sender;
    NSMutableString* ascii = [NSMutableString stringWithString:
                                  [[field stringValue] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]]];
    CFStringTransform((CFMutableStringRef)ascii, NULL, kCFStringTransformFullwidthHalfwidth, false);
    NSScanner* scanner = [NSScanner scannerWithString:ascii];
    double v = 0.0;
    stackcore::Levels& l = _levels[[self levelsChannel]];
    if (![scanner scanDouble:&v] || ![scanner isAtEnd] || !std::isfinite(v)) {
        NSBeep();
        [self updateLevelsControls];
        return;
    }
    const double gap = 2.0 / 255.0;
    if ([field tag] == 0) {
        l.black = std::max(0.0, std::min(v / 255.0, l.white - gap));
    } else if ([field tag] == 2) {
        l.white = std::min(1.0, std::max(v / 255.0, l.black + gap));
    } else {
        l.gamma = std::max(kLevelsGammaMin, std::min(kLevelsGammaMax, v));
    }
    [field abortEditing];
    [self finishingChanged:field];
}

// ［初期値に戻す］: RGB 全体と R・G・B すべてを 0 / 1.00 / 255 に戻す。
- (void)resetLevels:(id)sender {
    (void)sender;
    for (int i = 0; i < 4; ++i) _levels[i] = stackcore::Levels();
    [_levelsChannelPopup selectItemAtIndex:0];
    [self finishingChanged:nil];
}

// 設定の保存用: [[黒, 白, ガンマ] ×4]（RGB, R, G, B）。
- (NSArray*)levelsArray {
    NSMutableArray* a = [NSMutableArray array];
    for (int i = 0; i < 4; ++i) [a addObject:@[ @(_levels[i].black), @(_levels[i].white), @(_levels[i].gamma) ]];
    return a;
}

// 設定から読む。古いプリセット（tone・black・white・gamma）は RGB 全体に当てる。
- (void)setLevelsFromDictionary:(NSDictionary*)d {
    NSArray* a = d[@"levels"];
    if ([a isKindOfClass:[NSArray class]] && [a count] == 4) {
        for (int i = 0; i < 4; ++i) {
            NSArray* v = a[static_cast<NSUInteger>(i)];
            stackcore::Levels l;
            if ([v isKindOfClass:[NSArray class]] && [v count] == 3) {
                l.black = std::max(0.0, std::min(1.0, [v[0] doubleValue]));
                l.white = std::max(l.black + 1e-4, std::min(1.0, [v[1] doubleValue]));
                l.gamma = std::max(kLevelsGammaMin, std::min(kLevelsGammaMax, [v[2] doubleValue]));
            }
            _levels[i] = l;
        }
        return;
    }
    if (d[@"tone"] || d[@"black"]) {
        for (int i = 0; i < 4; ++i) _levels[i] = stackcore::Levels();
        if ([d[@"tone"] boolValue]) {
            _levels[0].black = [d[@"black"] doubleValue];
            _levels[0].white = std::max(_levels[0].black + 1e-4, d[@"white"] ? [d[@"white"] doubleValue] : 1.0);
            _levels[0].gamma = d[@"gamma"] ? std::max(kLevelsGammaMin, [d[@"gamma"] doubleValue]) : 1.0;
        }
    }
}

// ---- 向き・切り抜き -----------------------------------------------------------

// 枠は「いま画面に出ている画像」の座標なので、向きや表示を変えたら消す
// （そのまま残すと、見た目と違う場所を切り抜いてしまう）。
- (void)invalidateCropBox {
    if (_cropRect.size.width <= 0) return;
    _cropRect = NSZeroRect;
    [_preview setCropOverlay:NSZeroRect];
    [self updateCropControls];
}

- (void)rotateLeft:(id)sender {
    (void)sender;
    [self invalidateCropBox];
    _rotationTurns = (_rotationTurns + 3) % 4;
    [self finishingChanged:nil];
    [self updateApOverlay];
}

- (void)rotateRight:(id)sender {
    (void)sender;
    [self invalidateCropBox];
    _rotationTurns = (_rotationTurns + 1) % 4;
    [self finishingChanged:nil];
    [self updateApOverlay];
}

// ---- 切り抜き -------------------------------------------------------------------

// 枠の大きさと、切り抜いたかどうかを表示し、ボタンの有効・無効を合わせる。
- (void)updateCropControls {
    NSString* box = _cropRect.size.width > 0
        ? [NSString stringWithFormat:LSLocalizedString(@"枠: %.0f×%.0f px（x %.0f, y %.0f から）"),
                                     _cropRect.size.width, _cropRect.size.height, _cropRect.origin.x, _cropRect.origin.y]
        : LSLocalizedString(@"枠はまだありません");
    if (_uncroppedStacked && _stacked) {
        box = [box stringByAppendingFormat:LSLocalizedString(@" ／ 切り抜き済み %d×%d（元 %d×%d）"),
                                           _stacked->width(), _stacked->height(),
                                           _uncroppedStacked->width(), _uncroppedStacked->height()];
    }
    [_cropLabel setStringValue:box];
    [_cropApplyButton setEnabled:_stacked && _cropRect.size.width >= 2 && _cropRect.size.height >= 2];
    [_cropUndoButton setEnabled:_uncroppedStacked ? YES : NO];
}

- (void)cropModeChanged:(id)sender {
    (void)sender;
    const BOOL on = [_cropModeCheck state] == NSControlStateValueOn;
    if (on && _stacked) {
        // 枠はスタック結果（仕上げを掛けた見た目）の上に描く。AP配置の編集とは同時に使わない。
        if ([_viewModeSegment selectedSegment] != 2) {
            [_viewModeSegment setSelectedSegment:2];
            [self viewModeChanged:nil];
        }
        if ([_apEditCheck state] == NSControlStateValueOn) {
            [_apEditCheck setState:NSControlStateValueOff];
            [self apDisplayChanged:nil];
        }
        [[self window] makeFirstResponder:_preview];
    }
    [_preview setCropEditing:on && _stacked];
    [self updateApOverlay];
    [self updateCropControls];
}

- (void)previewView:(PreviewView*)view didChangeCropRect:(NSRect)rect {
    (void)view;
    _cropRect = rect;
    [self updateCropControls];
}

- (void)clearCropBox:(id)sender {
    (void)sender;
    _cropRect = NSZeroRect;
    [_preview setCropOverlay:NSZeroRect];
    [self updateCropControls];
}

// スタック結果（切り抜いたもの）を仕上げの入力にし直す。
//
// **処理系は作り直す。** 描画中の処理系に set_input するとスレッドが競合する。
// 番号を進めて、前の大きさで描いている途中の結果が後から表示されないようにする。
// 表示の明るさの基準（_stackedDisplayLow/High）は元のスタック結果のまま保つ
// （切り抜いて暗い空が減ると最小値が上がり、切り抜いた瞬間に明るさが変わってしまうため）。
- (void)replaceFinishingInput {
    ++_renderGeneration;
    _renderPending = NO;
    _renderTargets[0].reset();
    _renderTargets[1].reset();
    _displayed.reset();
    _finishing = std::make_shared<stackcore::FinishingPipeline>();
    _finishing->set_input(_stacked, kWaveletLayers);
    [self showFinishedOrStacked];
    [self requestFinishingRender];
    [self updateApOverlay];
    [self updateCropControls];
}

// 描いた枠でスタック結果を切り抜く（その場で画像が小さくなる。書き出しにもそのまま入る）。
- (void)applyCrop:(id)sender {
    (void)sender;
    if (!_stacked || _cropRect.size.width < 2 || _cropRect.size.height < 2) {
        [_statusLabel setStringValue:LSLocalizedString(@"先にプレビューで切り抜く枠を描いてください")];
        return;
    }
    // 枠は「いま画面に出ている画像」の座標。仕上げの効果をプレビューしているなら回転・反転の
    // 後の見た目なので、回転・反転の前（スタック結果）の座標に戻す。
    stackcore::Geometry shown;
    if ([_waveletPreviewCheck state] == NSControlStateValueOn) shown = [self currentFinishingSettings].geometry;
    int x = 0, y = 0, w = 0, h = 0;
    stackcore::geometry_output_rect_to_input(shown, _stacked->width(), _stacked->height(),
                                             static_cast<int>(_cropRect.origin.x), static_cast<int>(_cropRect.origin.y),
                                             static_cast<int>(_cropRect.size.width), static_cast<int>(_cropRect.size.height),
                                             x, y, w, h);
    auto cut = std::make_shared<stackcore::FrameBuffer>();
    stackcore::crop_frame(*_stacked, x, y, w, h, *cut);
    if (!_uncroppedStacked) {
        _uncroppedStacked = _stacked;
        _appliedCrop = NSMakeRect(x, y, w, h);
    } else {
        // 切り抜きを重ねた。元のスタック結果の座標で持つ（まとめて書き出しで使う）。
        _appliedCrop = NSMakeRect(_appliedCrop.origin.x + x, _appliedCrop.origin.y + y, w, h);
    }
    _stacked = cut;
    _cropRect = NSZeroRect;
    [_preview setCropOverlay:NSZeroRect];
    [_cropModeCheck setState:NSControlStateValueOff];
    [_preview setCropEditing:NO];
    [self replaceFinishingInput];
    [_statusLabel setStringValue:[NSString stringWithFormat:LSLocalizedString(@"切り抜きました — %d×%d"), w, h]];
}

- (void)undoCrop:(id)sender {
    (void)sender;
    if (!_uncroppedStacked) return;
    _stacked = _uncroppedStacked;
    _uncroppedStacked.reset();
    _appliedCrop = NSZeroRect;
    _cropRect = NSZeroRect;
    [_preview setCropOverlay:NSZeroRect];
    [self replaceFinishingInput];
    [_statusLabel setStringValue:[NSString stringWithFormat:LSLocalizedString(@"切り抜く前に戻しました — %d×%d"),
                                                            _stacked->width(), _stacked->height()]];
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
    if (!f.levels.identity()) {
        const char* names[4] = {"RGB", "R", "G", "B"};
        const stackcore::Levels* all[4] = {&f.levels.master, &f.levels.channel[0], &f.levels.channel[1], &f.levels.channel[2]};
        for (int i = 0; i < 4; ++i) {
            if (all[i]->identity()) continue;
            std::snprintf(buf, sizeof(buf), "levels %s black %.4f white %.4f gamma %.2f", names[i], all[i]->black,
                          all[i]->white, all[i]->gamma);
            m.history.push_back(buf);
        }
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
    stackcore::FinishingSettings finishing = [self currentFinishingSettings];
    // 画面で切り抜いた範囲は、ほかの枚数のスタックにも同じ位置で掛ける（回転の前に切る）。
    if (_uncroppedStacked) {
        finishing.geometry.crop = true;
        finishing.geometry.crop_x = static_cast<int>(_appliedCrop.origin.x);
        finishing.geometry.crop_y = static_cast<int>(_appliedCrop.origin.y);
        finishing.geometry.crop_width = static_cast<int>(_appliedCrop.size.width);
        finishing.geometry.crop_height = static_cast<int>(_appliedCrop.size.height);
    }
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
