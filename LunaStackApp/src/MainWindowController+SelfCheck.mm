#import "MainWindowController_Private.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// GUIを人が操作しなくても主要な経路を通し、壊れていないかを確かめるための仕掛け。
// AppDelegate が環境変数 LUNASTACK_* を見て呼ぶ。通常の操作では使わない。
@implementation MainWindowController (SelfCheck)

// APの当たり判定を自己検証する。
//
// **描画の座標変換と当たり判定の座標変換がずれる**のが、この手のUIで
// いちばん気づきにくい壊れ方である。枠は正しく見えているのに
// クリックすると別のAPが選ばれる、という形で出る。
// 画面に描いた位置をそのまま押したとき、同じAPが返るかを全点で確かめる。
- (BOOL)selfCheckApHitTest {
    if (!_analysis || _analysis->points.empty()) {
        NSLog(@"AP当たり判定の自己検証: APが無いので確認できません");
        return NO;
    }
    const double coordScale = [self overlayScale];

    int checked = 0, mismatched = 0;
    double maxRoundTrip = 0.0;
    for (std::size_t i = 0; i < _analysis->points.size(); ++i) {
        const NSPoint img = NSMakePoint(_analysis->points[i].cx * coordScale,
                                        _analysis->points[i].cy * coordScale);
        const NSPoint view = [_preview viewPointFromImagePoint:img];
        const NSPoint back = [_preview imagePointFromViewPoint:view];
        maxRoundTrip = std::max(maxRoundTrip,
                                std::max(std::fabs(back.x - img.x), std::fabs(back.y - img.y)));

        const NSInteger hit = [_preview apIndexAtViewPoint:view];
        if (hit != static_cast<NSInteger>(i)) ++mismatched;
        ++checked;
    }
    NSLog(@"AP当たり判定の自己検証: %d点中 一致しない %d点 / 往復誤差 最大 %.4f px（倍率 %.2f）", checked,
          mismatched, maxRoundTrip, [_preview effectiveDeviceZoom]);
    return mismatched == 0 && maxRoundTrip < 0.01;
}

// AP編集とプリセットの往復を自己検証する。
//
// どちらも「画面のつまみ → 設定 → 保存 → 復元」の受け渡しであり、
// 途中の1項目を書き忘れても画面上は何も起きない。気づくのは
// 「プリセットを読んだのに前と結果が違う」という遠い場所になる。
- (BOOL)selfCheckEditingAndPresets {
    BOOL ok = YES;

    // --- AP編集（取り消しを含む） ---
    if (_analysis && !_analysis->points.empty()) {
        const std::size_t before = _analysis->points.size();
        NSString* signatureBefore = [[self analysisSignature] copy];
        NSUndoManager* undo = [[self window] undoManager];
        // 自己検証はイベント処理の外から呼ばれるので、取り消しの区切りを自分で付ける。
        [undo setGroupsByEvent:NO];

        [undo beginUndoGrouping];
        [self previewView:_preview didDeleteApAtIndex:0];
        [undo endUndoGrouping];
        [undo beginUndoGrouping];
        [self previewView:_preview didAddApAtX:10 y:10];
        [undo endUndoGrouping];

        const stackcore::MapStackSettings settings = [self currentSettings];
        if (!settings.ap.use_manual_points) {
            NSLog(@"AP編集の自己検証: 手動配置が有効になっていません");
            ok = NO;
        }
        if (settings.ap.manual_points.size() != before) {
            // 1つ消して1つ足したので数は変わらない
            NSLog(@"AP編集の自己検証: AP数が合いません（%zu → %zu、期待 %zu）", before,
                  settings.ap.manual_points.size(), before);
            ok = NO;
        }
        if ([signatureBefore isEqualToString:[self analysisSignature]]) {
            NSLog(@"AP編集の自己検証: APを変えたのに解析が無効になっていません");
            ok = NO;
        }
        // 2回取り消せば元の状態（自動配置・解析が有効）に戻る。
        [undo undo];
        [undo undo];
        if (_manualPointsActive || ![signatureBefore isEqualToString:[self analysisSignature]]) {
            NSLog(@"AP編集の自己検証: 取り消しで元に戻りません");
            ok = NO;
        }
        [undo redo];
        if (!_manualPointsActive || _manualPoints.size() != before - 1) {
            NSLog(@"AP編集の自己検証: やり直しが効きません");
            ok = NO;
        }
        [signatureBefore release];

        // 元に戻す
        _manualPointsActive = NO;
        _manualPoints.clear();
        [undo removeAllActionsWithTarget:self];
        [undo setGroupsByEvent:YES];
        [self updateApOverlay];
        [self updateControlsEnabled];
    }

    // --- プリセット ---
    NSDictionary* original = [self settingsDictionary];
    [_apTopSlider setDoubleValue:37.0];
    [_topSlider setDoubleValue:41.0];
    [_sharpenSliders[2] setDoubleValue:2.25];
    [_denoiseSliders[4] setDoubleValue:0.55];
    [_drizzleSegment setSelectedSegment:2];
    [_endianPopup selectItemAtIndex:2];
    [_debayerPopup selectItemAtIndex:1];
    [_gainSliders[2] setDoubleValue:1.2];
    [_minScoreField setStringValue:@"0.42"];
    [_nameStylePopup selectItemAtIndex:1];
    NSDictionary* modified = [self settingsDictionary];

    NSString* error = nil;
    NSString* name = @"__self_check__";
    if (![Presets saveSettings:modified name:name error:&error]) {
        NSLog(@"プリセットの自己検証: 保存に失敗 %@", error);
        ok = NO;
    } else {
        [self applySettingsDictionary:original];
        NSDictionary* loaded = [Presets loadSettingsNamed:name];
        [self applySettingsDictionary:loaded];
        NSDictionary* restored = [self settingsDictionary];
        for (NSString* key in modified) {
            if (![[restored[key] description] isEqualToString:[modified[key] description]]) {
                NSLog(@"プリセットの自己検証: %@ が戻りません（%@ → %@）", key, modified[key],
                      restored[key]);
                ok = NO;
            }
        }
        [Presets removeSettingsNamed:name];
        [self applySettingsDictionary:original];
    }

    NSLog(@"AP編集・プリセットの自己検証: %@", ok ? @"問題なし" : @"問題あり");
    return ok;
}

// スライダーの品質順が、エンジンの上位選択（select_top_frames）と同じ並びかを確かめる。
// ここがずれると「スライダーで上位10%に見えたフレーム」と「実際に使われたフレーム」が食い違う。
- (BOOL)selfCheckFrameOrder {
    if (_frameInfos.empty() || _qualityOrder.empty()) {
        NSLog(@"フレーム順の自己検証: 品質評価の結果がありません");
        return NO;
    }
    BOOL ok = YES;
    int accepted = 0;
    for (const stackcore::FrameInfo& f : _frameInfos) {
        if (f.accepted) ++accepted;
    }
    for (double percent : {5.0, 10.0, 25.0, 50.0, 100.0}) {
        const std::vector<stackcore::FrameInfo> top = stackcore::select_top_frames(_frameInfos, percent);
        std::vector<int> expected;
        for (const stackcore::FrameInfo& f : top) expected.push_back(f.index);
        std::vector<int> fromOrder(_qualityOrder.begin(), _qualityOrder.begin() + static_cast<std::ptrdiff_t>(top.size()));
        std::sort(fromOrder.begin(), fromOrder.end());
        if (fromOrder != expected) {
            NSLog(@"フレーム順の自己検証: 上位%.0f%%がエンジンの選択と一致しません", percent);
            ok = NO;
        }
    }
    // 品質順のスライダーの左端は最良のフレーム、右端に除外フレームが並ぶ。
    const BOOL savedOrder = _frameOrderByQuality;
    const double savedPosition = [_frameSlider doubleValue];
    const NSInteger savedMode = [_viewModeSegment selectedSegment];
    [_viewModeSegment setSelectedSegment:0];
    [self setFrameOrderByQuality:YES];
    [_frameSlider setDoubleValue:0.0];
    const int best = [self currentFrameIndex];
    for (const stackcore::FrameInfo& f : _frameInfos) {
        if (f.accepted && f.quality > _frameInfos[static_cast<std::size_t>(best)].quality) {
            NSLog(@"フレーム順の自己検証: 左端が最良のフレームではありません");
            ok = NO;
            break;
        }
    }
    [self updateFrameInfoLabel];
    NSString* label = [_frameInfoLabel stringValue];
    NSString* expectTop = [NSString stringWithFormat:LSLocalizedString(@"上位 %.1f%% · %@ · 品質 %.4g"),
                                                     100.0 / std::max(1, accepted),
                                                     [NSString stringWithFormat:@"#%d / %d",
                                                                                _previewSource->original_index(best) + 1,
                                                                                std::max(_sourceTotalFrames, _sourceFrames)],
                                                     _frameInfos[static_cast<std::size_t>(best)].quality];
    if (![label isEqualToString:expectTop]) {
        NSLog(@"フレーム順の自己検証: 表示が想定と違います（%@）", label);
        ok = NO;
    }
    // 除外フレームは品質順の最後に並ぶ。
    BOOL seenRejected = NO;
    for (int k : _qualityOrder) {
        const BOOL rejected = !_frameInfos[static_cast<std::size_t>(k)].accepted;
        if (seenRejected && !rejected) {
            NSLog(@"フレーム順の自己検証: 除外フレームの後に採用フレームがあります");
            ok = NO;
            break;
        }
        seenRejected = seenRejected || rejected;
    }
    [self setFrameOrderByQuality:savedOrder];
    [_frameSlider setDoubleValue:savedPosition];
    [_viewModeSegment setSelectedSegment:savedMode];
    [self updateFrameInfoLabel];
    NSLog(@"フレーム順の自己検証: %@（採用 %d / 全 %zu フレーム）", ok ? @"一致" : @"不一致", accepted,
          _frameInfos.size());
    return ok;
}

// 画面の仕上げと書き出す画像が、同じ設定なら同じ画素になるかを確かめる。
- (BOOL)selfCheckFinishingMatchesExport {
    if (!_stacked || !_finishing) {
        NSLog(@"仕上げの自己検証: スタック結果がありません");
        return NO;
    }
    std::shared_ptr<stackcore::FrameBuffer> shown = [self renderFinishingNow];
    stackcore::FinishingPipeline fresh;
    fresh.set_input(_stacked, kWaveletLayers);
    stackcore::FrameBuffer exported;
    fresh.render([self currentFinishingSettings], exported);
    BOOL ok = shown->width() == exported.width() && shown->height() == exported.height() &&
              shown->channels() == exported.channels();
    for (int c = 0; ok && c < exported.channels(); ++c) {
        for (int y = 0; ok && y < exported.height(); ++y) {
            ok = std::memcmp(shown->row(c, y), exported.row(c, y), sizeof(float) * exported.width()) == 0;
        }
    }
    NSLog(@"仕上げの自己検証: 画面と書き出しが%@（%d×%d）", ok ? @"一致" : @"不一致", exported.width(),
          exported.height());
    return ok;
}

// 入力の読み方の欄に触れただけ（値は変えない）で、スタック結果が消えないかを確かめる。
// 数値欄は入力欄を離れたときにも action を送るので、クリックして離れるだけで呼ばれる。
- (BOOL)selfCheckUntouchedInputKeepsResult {
    if (!_stacked) {
        NSLog(@"入力欄の自己検証: スタック結果がありません");
        return NO;
    }
    [self inputInterpretationChanged:_rangeStartField];
    [self inputInterpretationChanged:_bayerPopup];
    const BOOL ok = _stacked != nullptr && [self alignmentUsable];
    NSLog(@"入力欄の自己検証: 値を変えずに離れたとき結果が%@", ok ? @"残る" : @"消えた");
    return ok;
}

// 「ウェーブレットの効果をプレビュー」のON/OFFで表示が変わり、OFFのときは
// ウェーブレットを外した仕上げと同じ画素になるか。書き出しはOFFでも効果を含むか。
- (BOOL)selfCheckWaveletPreviewToggle {
    if (!_stacked || !_finishing) return NO;
    [_sharpenSliders[0] setDoubleValue:4.0];
    [self updateFinishingValueLabels];
    [_waveletOnlyPreviewCheck setState:NSControlStateValueOn];
    std::shared_ptr<stackcore::FrameBuffer> on = [self renderFinishingNowWithSettings:[self previewFinishingSettings]];
    [_waveletOnlyPreviewCheck setState:NSControlStateValueOff];
    std::shared_ptr<stackcore::FrameBuffer> off = [self renderFinishingNowWithSettings:[self previewFinishingSettings]];
    stackcore::FinishingSettings none = [self currentFinishingSettings];
    none.wavelet.assign(kWaveletLayers, stackcore::WaveletLayerParams());
    none.dering = 0.0;
    std::shared_ptr<stackcore::FrameBuffer> expected = [self renderFinishingNowWithSettings:none];
    std::shared_ptr<stackcore::FrameBuffer> exported = [self renderFinishingNow];
    const auto same = [](const stackcore::FrameBuffer& a, const stackcore::FrameBuffer& b) {
        if (a.width() != b.width() || a.height() != b.height() || a.channels() != b.channels()) return false;
        for (int c = 0; c < a.channels(); ++c) {
            for (int y = 0; y < a.height(); ++y) {
                if (std::memcmp(a.row(c, y), b.row(c, y), sizeof(float) * a.width()) != 0) return false;
            }
        }
        return true;
    };
    const BOOL ok = !same(*on, *off) && same(*off, *expected) && same(*on, *exported);
    [_waveletOnlyPreviewCheck setState:NSControlStateValueOn];
    [_sharpenSliders[0] setDoubleValue:1.0];
    [self updateFinishingValueLabels];
    NSLog(@"ウェーブレット切り替えの自己検証: %@", ok ? @"ON/OFFで表示が変わり、書き出しには効果が入る" : @"想定と違う");
    return ok;
}

// ±ボタン・数値欄・レイヤーの「初期値に戻す」が、つまみと仕上げの設定に正しく効くか。
- (BOOL)selfCheckWaveletControls {
    BOOL ok = YES;
    const auto expect = [&ok](double got, double want, NSString* what) {
        if (std::fabs(got - want) > 1e-9) {
            NSLog(@"ウェーブレット操作の自己検証: %@ が %.4f（期待 %.4f）", what, got, want);
            ok = NO;
        }
    };
    [_linkedCheck setState:NSControlStateValueOff];
    [self resetWavelet:nil];
    [self adjustPlus:_adjPlus[0]];
    expect([_sharpenSliders[0] doubleValue], 1.05, @"＋1回後の強調");
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.25]];
    [self adjustMinus:_adjMinus[1]];  // ノイズは0より下がらない
    expect([_denoiseSliders[0] doubleValue], 0.0, @"－後のノイズ（下限）");
    [_sharpenValues[1] setStringValue:@"3.5"];
    [self adjustFieldChanged:_sharpenValues[1]];
    expect([_sharpenSliders[1] doubleValue], 3.5, @"数値入力した強調");
    [_denoiseValues[1] setStringValue:@"０．４"];  // 全角でも読める
    [self adjustFieldChanged:_denoiseValues[1]];
    expect([_denoiseSliders[1] doubleValue], 0.4, @"全角で入力したノイズ");
    [_sharpenValues[2] setStringValue:@"50"];  // 範囲外は上限に収める
    [self adjustFieldChanged:_sharpenValues[2]];
    expect([_sharpenSliders[2] doubleValue], kWaveletGuiSharpenMaximum, @"範囲外の入力");
    [_sharpenValues[2] setStringValue:@"abc"];  // 読めない値は元のまま
    [self adjustFieldChanged:_sharpenValues[2]];
    expect([_sharpenSliders[2] doubleValue], kWaveletGuiSharpenMaximum, @"不正な入力");
    const stackcore::FinishingSettings s = [self currentFinishingSettings];
    expect(s.wavelet[1].sharpen, 3.5, @"仕上げの設定に入った強調");
    expect(s.wavelet[1].denoise, 0.4, @"仕上げの設定に入ったノイズ");
    [self resetLayer:_layerResetButtons[1]];
    expect([_sharpenSliders[1] doubleValue], 1.0, @"初期値に戻した強調");
    expect([_denoiseSliders[1] doubleValue], 0.0, @"初期値に戻したノイズ");
    expect([_sharpenSliders[2] doubleValue], kWaveletGuiSharpenMaximum, @"ほかのレイヤー（戻さない）");
    [self resetWavelet:nil];
    [self waitForFinishingForTesting];
    NSLog(@"ウェーブレット操作の自己検証: %@", ok ? @"問題なし" : @"問題あり");
    return ok;
}

// スライダーをドラッグしている間も、プレビューが途切れずに更新され続けるか。
// 60Hz相当で値を変えながら描画を要求し、その間に画面へ出た回数を数える。
namespace {

// 高周波の量（輝度の4近傍ラプラシアンの絶対値の平均）。ウェーブレットの強調が
// 画面に出ているかを数値で見る。
double HighFrequency(const stackcore::FrameBuffer& f) {
    if (f.width() < 3 || f.height() < 3) return 0.0;
    // 大きな画像では間引いて数える（検査そのものが画面の更新を遅らせないように）。
    const int step = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(f.width()) * f.height() / 250000.0)));
    double sum = 0.0;
    std::size_t n = 0;
    for (int y = 1; y + 1 < f.height(); y += step) {
        for (int x = 1; x + 1 < f.width(); x += step) {
            double lap = 0.0;
            for (int c = 0; c < f.channels(); ++c) {
                const float* r = f.row(c, y);
                lap += 4.0 * r[x] - r[x - 1] - r[x + 1] - f.row(c, y - 1)[x] - f.row(c, y + 1)[x];
            }
            sum += std::fabs(lap);
            ++n;
        }
    }
    return n > 0 ? sum / n : 0.0;
}

}  // namespace

// つまみを動かし続けても、画面が途切れず、しかも「動かす前 → 動かした後」とだけ
// 移り変わるか。強調を上げていくだけの操作なので、途中で強調の無い画像
// （スタックそのまま・縮小版の下書き）が一瞬でも出れば高周波の量が下がる。
- (BOOL)selfCheckContinuousPreview {
    if (!_stacked || !_finishing) return NO;
    [_viewModeSegment setSelectedSegment:2];
    [_waveletPreviewCheck setState:NSControlStateValueOn];
    [_sharpenSliders[0] setDoubleValue:2.0];
    [self updateFinishingValueLabels];
    [self waitForFinishingForTesting];
    const double before = _displayed ? HighFrequency(*_displayed) : 0.0;

    double lowest = 1e300;
    double previous = before;
    int drops = 0;
    _previewUpdateHook = [&](const stackcore::FrameBuffer& shown) {
        const double hf = HighFrequency(shown);
        lowest = std::min(lowest, hf);
        // 強調は単調に上げているので、表示の高周波も下がらないはず（丸めの揺れは許す）。
        if (hf < previous * 0.999) ++drops;
        previous = hf;
    };
    _previewUpdates = 0;
    const int steps = 60;
    const NSTimeInterval dragStart = [NSDate timeIntervalSinceReferenceDate];
    for (int i = 0; i < steps; ++i) {
        [_sharpenSliders[0] setDoubleValue:2.0 + 8.0 * (i + 1) / steps];
        [self waveletChanged:_sharpenSliders[0]];
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.0 / 60.0]];
    }
    const int duringDrag = _previewUpdates;
    const double dragSeconds = [NSDate timeIntervalSinceReferenceDate] - dragStart;
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
    _previewUpdateHook = nullptr;
    [_sharpenSliders[0] setDoubleValue:1.0];
    [self updateFinishingValueLabels];
    [self waitForFinishingForTesting];
    // 1秒のドラッグで10回以上（＝おおむね10fps以上）描き直していれば「途切れない」とみなす。
    // 400万画素を超える画像（一眼レフのRAWなど）は、仕上げ1回に0.1秒以上かかり、メモリの
    // 込み具合で速さが揺れる（6240×4160 で毎秒3〜8回）。そこでは「止まらずに更新が続く」
    // （ドラッグ中に2回以上）ことを見る。0回（ドラッグ中ずっと止まる）が以前の不具合。
    const double pixels = static_cast<double>(_stacked->width()) * _stacked->height();
    const int required = pixels > 4.0e6 ? 2 : 10;
    const BOOL smooth = duringDrag >= required;
    const BOOL monotonic = drops == 0 && lowest >= before * 0.999;
    NSLog(@"連続プレビューの自己検証: ドラッグ中 %d 回更新（%d 回の操作・%.2f 秒、%d×%d）、高周波 動かす前 %.5f / "
          @"途中の最小 %.5f、下がった回数 %d %@",
          duringDrag, steps, dragSeconds, _stacked->width(), _stacked->height(), before, lowest, drops,
          !smooth ? @"— 途切れています" : (!monotonic ? @"— 強調の無い画像が挟まっています" : @""));
    return smooth && monotonic;
}

// 明るさの対応（黒点・白点のON/OFFなど）を変えても、新しい仕上げが描き上がるまでは
// 画面の対応を変えないか（前の画像だけ明るさが変わって一瞬ちらつかないように）。
- (BOOL)selfCheckMappingWaitsForResult {
    if (!_stacked || !_finishing) return NO;
    [_viewModeSegment setSelectedSegment:2];
    [_waveletPreviewCheck setState:NSControlStateValueOn];
    [self waitForFinishingForTesting];
    LSDisplayMapping before;
    const BOOL fixedBefore = [_preview currentDisplayMapping:&before];
    const stackcore::Levels saved = _levels[0];
    _levels[0].white = saved.identity() ? 0.5 : 1.0;  // 初期値との間で切り替える
    if (!saved.identity()) _levels[0] = stackcore::Levels();
    [self finishingChanged:nil];  // ランループは回さない（まだ描き上がっていない）
    LSDisplayMapping during;
    const BOOL fixedDuring = [_preview currentDisplayMapping:&during];
    const BOOL ok = fixedBefore == fixedDuring && (!fixedBefore || before == during);
    _levels[0] = saved;
    [self finishingChanged:nil];
    [self waitForFinishingForTesting];
    NSLog(@"表示の対応の自己検証: %@", ok ? @"描き上がるまで変わらない" : @"— 描き上がる前に変わっています");
    return ok;
}

// 回転した見た目の上で描いた枠で切り抜くと、画面の見た目どおりの範囲が切り出され、
// 書き出しも画面と一致し、［元に戻す］で元のスタック結果にそっくり戻るか。
- (BOOL)selfCheckCropApplies {
    if (!_stacked || !_finishing) return NO;
    std::shared_ptr<stackcore::FrameBuffer> original = _stacked;
    std::shared_ptr<stackcore::FrameBuffer> alreadyCropped = _uncroppedStacked;
    const NSRect appliedBefore = _appliedCrop;
    [_viewModeSegment setSelectedSegment:2];
    [_waveletPreviewCheck setState:NSControlStateValueOn];
    const int turnsBefore = _rotationTurns;
    _rotationTurns = 1;
    [self finishingChanged:nil];
    // 比べるために、近傍を使う処理（ウェーブレット・チャンネル合わせ）を外した見た目で比べる。
    stackcore::FinishingSettings pointwise = [self previewFinishingSettings];
    pointwise.wavelet.assign(kWaveletLayers, stackcore::WaveletLayerParams());
    pointwise.dering = 0.0;
    pointwise.channels = stackcore::ChannelOffsets();
    std::shared_ptr<stackcore::FrameBuffer> before = [self renderFinishingNowWithSettings:pointwise];
    const int bx = before->width() / 5, by = before->height() / 7;
    const int bw = std::max(2, before->width() / 2), bh = std::max(2, before->height() / 3);
    _cropRect = NSMakeRect(bx, by, bw, bh);
    [self applyCrop:nil];
    [self waitForFinishingForTesting];
    BOOL ok = _displayed && _displayed->width() == bw && _displayed->height() == bh;
    // 切り抜いた後の見た目 = 切り抜く前の見た目の、枠の中。
    std::shared_ptr<stackcore::FrameBuffer> after = [self renderFinishingNowWithSettings:pointwise];
    double worst = 0.0;
    for (int c = 0; ok && c < after->channels(); ++c) {
        for (int y = 0; y < bh; ++y) {
            for (int x = 0; x < bw; ++x) {
                worst = std::max(worst, static_cast<double>(std::fabs(after->row(c, y)[x] - before->row(c, by + y)[bx + x])));
            }
        }
    }
    ok = ok && worst < 1e-6;
    // 書き出しと画面が一致する。
    std::shared_ptr<stackcore::FrameBuffer> exported = [self renderFinishingNow];
    std::shared_ptr<stackcore::FrameBuffer> shown = [self renderFinishingNowWithSettings:[self previewFinishingSettings]];
    ok = ok && exported->width() == shown->width() && exported->height() == shown->height();
    // 枠を描いたあとに向きを変えると、枠は消える（見た目と違う場所を切り抜かない）。
    _cropRect = NSMakeRect(1, 1, 4, 4);
    [self rotateRight:nil];
    const BOOL boxCleared = _cropRect.size.width == 0;
    [self rotateLeft:nil];
    ok = ok && boxCleared;
    // 元に戻す。
    [self undoCrop:nil];
    const BOOL restored = _stacked == (alreadyCropped ? alreadyCropped : original) && !_uncroppedStacked;
    ok = ok && restored;
    if (alreadyCropped) {
        // 自己検証の前から切り抜いてあった場合は、その状態に戻しておく。
        _uncroppedStacked = alreadyCropped;
        _stacked = original;
        _appliedCrop = appliedBefore;
        [self replaceFinishingInput];
    }
    _rotationTurns = turnsBefore;
    [self finishingChanged:nil];
    [self waitForFinishingForTesting];
    NSLog(@"切り抜きの自己検証: 枠 %d×%d、画面との差 最大 %.2g、回転で枠を消す %@、元に戻す %@ %@", bw, bh, worst,
          boxCleared ? @"OK" : @"NG", restored ? @"OK" : @"NG", ok ? @"" : @"— 合いません");
    return ok;
}

- (void)setRoiForTesting:(NSString*)spec {
    if ([spec isEqualToString:@"center"]) {
        [self commitRoiRect:NSMakeRect(_sourceWidth / 4, _sourceHeight / 4, _sourceWidth / 2, _sourceHeight / 2)];
        return;
    }
    NSArray* v = [spec componentsSeparatedByString:@":"];
    if ([v count] == 4) {
        [self commitRoiRect:NSMakeRect([v[0] doubleValue], [v[1] doubleValue], [v[2] doubleValue], [v[3] doubleValue])];
    }
}

// 処理範囲: フレーム表示は全体のまま、処理に使う入力は範囲だけになり、範囲を変えると
// 解析結果は使えなくなり、［全体に戻す］で元に戻るか。範囲の外で描いた枠は画像の中に収まるか。
- (BOOL)selfCheckRoi {
    if (!_previewSource) return NO;
    const NSRect before = _roiRect;
    NSString* signatureBefore = [[[self analysisSignature] copy] autorelease];
    // 奇数の位置・はみ出す大きさで描いても、偶数に揃って画像の中に収まる。
    [self commitRoiRect:NSMakeRect(_sourceWidth / 3 + 1, _sourceHeight / 3 + 1, _sourceWidth, _sourceHeight / 3)];
    BOOL ok = _roiRect.size.width > 0 && static_cast<int>(_roiRect.origin.x) % 2 == 0 &&
              static_cast<int>(_roiRect.origin.y) % 2 == 0 && NSMaxX(_roiRect) <= _sourceWidth &&
              NSMaxY(_roiRect) <= _sourceHeight;
    ok = ok && (![[self analysisSignature] isEqualToString:signatureBefore] || NSEqualRects(before, _roiRect));
    // 処理に使う入力は範囲の大きさ、フレーム表示は全体。中身は全体の範囲の部分と一致する。
    try {
        std::unique_ptr<stackcore::VideoSource> roi = stackcore::open_video(_inputPath, [self currentOpenOptions]);
        ok = ok && roi->width() == static_cast<int>(_roiRect.size.width) &&
             roi->height() == static_cast<int>(_roiRect.size.height) && _previewSource->width() == _sourceWidth;
        stackcore::FrameBuffer a, b;
        roi->read_frame(0, a);
        _previewSource->read_frame(0, b);
        const int ox = static_cast<int>(_roiRect.origin.x), oy = static_cast<int>(_roiRect.origin.y);
        for (int c = 0; ok && c < a.channels(); ++c) {
            for (int y = 0; ok && y < a.height(); y += 7) {
                for (int x = 0; x < a.width(); x += 7) {
                    if (a.row(c, y)[x] != b.row(c, oy + y)[ox + x]) ok = NO;
                }
            }
        }
    } catch (const std::exception&) {
        ok = NO;
    }
    [self clearRoi:nil];
    ok = ok && _roiRect.size.width == 0;
    // 元の状態に戻す。
    if (before.size.width > 0) [self commitRoiRect:before];
    NSLog(@"処理範囲の自己検証: %@", ok ? @"範囲どおりに切り出し、全体に戻せる" : @"— 合いません");
    return ok;
}

// ←→ のボタンでスライダーの並びどおりに1枚ずつ送れ、端では止まるか。
- (BOOL)selfCheckFrameStepButtons {
    if (_sourceFrames < 2) return YES;
    const double saved = [_frameSlider doubleValue];
    [_frameSlider setDoubleValue:0.0];
    NSButton* forward = [[[NSButton alloc] init] autorelease];
    [forward setTag:1];
    NSButton* back = [[[NSButton alloc] init] autorelease];
    [back setTag:-1];
    [self frameStepButton:forward];
    const BOOL moved = std::lround([_frameSlider doubleValue]) == 1;
    [self frameStepButton:back];
    [self frameStepButton:back];
    const BOOL clamped = std::lround([_frameSlider doubleValue]) == 0 && [_viewModeSegment selectedSegment] == 0;
    [_frameSlider setDoubleValue:saved];
    [self frameSliderChanged:nil];
    NSLog(@"コマ送りの自己検証: 進む %@、端で止まる %@", moved ? @"OK" : @"NG", clamped ? @"OK" : @"NG");
    return moved && clamped;
}

// 右の設定パネルに横スクロール（トラックパッドの横スワイプ）を送っても、左右へずれないか。
- (BOOL)selfCheckInspectorScrollsVerticallyOnly {
    NSClipView* clip = [_inspectorScroll contentView];
    [[_inspectorScroll window] layoutIfNeeded];
    BOOL ok = YES;
    for (int i = 0; i < 2; ++i) {
        // 横方向（第2軸）に大きく動かす。1回目は左へ、2回目は右へ。
        const int32_t dx = i == 0 ? -120 : 120;
        CGEventRef cg = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitPixel, 2, 0, dx);
        NSEvent* event = cg ? [NSEvent eventWithCGEvent:cg] : nil;
        if (cg) CFRelease(cg);
        if (event) [_inspectorScroll scrollWheel:event];
        // 直接動かそうとしても x は 0 に戻されること。
        [clip scrollToPoint:NSMakePoint(i == 0 ? 40.0 : -40.0, [clip bounds].origin.y)];
        [_inspectorScroll reflectScrolledClipView:clip];
        if ([clip bounds].origin.x != 0.0) ok = NO;
    }
    const CGFloat docWidth = NSWidth([[_inspectorScroll documentView] frame]);
    const CGFloat clipWidth = NSWidth([clip bounds]);
    if (docWidth > clipWidth + 0.5) ok = NO;
    NSLog(@"設定パネルの自己検証（%@）: 横位置 %.1f、中身の幅 %.1f / 表示幅 %.1f %@", NSStringFromClass([clip class]), [clip bounds].origin.x,
          docWidth, clipWidth, ok ? @"（横に動かない）" : @"— 横に動きます");
    return ok;
}

// スタック結果を表示した直後は、位置合わせ領域の枠が消えているか。
- (BOOL)selfCheckApHiddenAfterStack {
    const BOOL ok = [[_displayMenu itemAtIndex:1] state] == NSControlStateValueOff &&
                    [_viewModeSegment selectedSegment] == 2;
    NSLog(@"枠の自己検証: スタック結果の表示で位置合わせ領域の枠が%@", ok ? @"消えている" : @"残っている");
    return ok;
}

// ［クリア］で入力キュー・設定・仕上げのつまみがすべて起動直後の状態に戻るか。
- (BOOL)selfCheckClearResetsEverything {
    // いろいろ動かしてから消す。
    [_sharpenSliders[0] setDoubleValue:7.5];
    [_denoiseSliders[1] setDoubleValue:0.4];
    [_gainSliders[0] setDoubleValue:1.3];
    [_saturationSlider setDoubleValue:1.6];
    [_deringSlider setDoubleValue:0.7];
    _levels[0].black = 0.1;
    _levels[2].gamma = 1.7;
    [_levelsChannelPopup selectItemAtIndex:2];
    [_channelFields[1] setStringValue:@"1.25"];
    [_topSlider setDoubleValue:40.0];
    [_drizzleSegment setSelectedSegment:3];
    [_bayerPopup selectItemAtIndex:2];
    [_rangeStartField setStringValue:@"3"];
    [_minScoreField setStringValue:@"0.3"];
    [_formatPopup selectItemAtIndex:2];
    [_objectField setStringValue:@"Mars"];
    _rotationTurns = 2;
    [_flipHCheck setState:NSControlStateValueOn];
    [_waveletOnlyPreviewCheck setState:NSControlStateValueOff];
    [self clearWorkspace:nil];

    BOOL ok = [_items count] == 0 && _rotationTurns == 0 && _cropRect.size.width == 0 && !_uncroppedStacked &&
              !_darkPath && !_flatPath && !_stacked &&
              [_waveletOnlyPreviewCheck state] == NSControlStateValueOn &&
              [_waveletPreviewCheck state] == NSControlStateValueOn;
    NSDictionary* now = [self settingsDictionary];
    for (NSString* key in _defaultSettings) {
        if (![[now[key] description] isEqualToString:[_defaultSettings[key] description]]) {
            NSLog(@"クリアの自己検証: %@ が初期値に戻りません（%@ → %@）", key, _defaultSettings[key], now[key]);
            ok = NO;
        }
    }
    NSLog(@"クリアの自己検証: %@", ok ? @"すべて初期値" : @"初期値に戻らない項目あり");
    return ok;
}

- (void)waitForFinishingForTesting {
    if (!_finishing) return;
    std::shared_ptr<stackcore::FrameBuffer> out =
        [self renderFinishingNowWithSettings:[self previewFinishingSettings]];
    _displayed = out;
    ++_renderGeneration;  // 途中の非同期描画で上書きさせない
    if ([_viewModeSegment selectedSegment] == 2) [self showFinishedOrStacked];
}

// "channel=auto,wb=auto,crop=auto,rotate=1,dering=0.6,tone=auto,linked=1.5" のような指定で
// 仕上げのつまみを動かす（ボタンを押したのと同じ経路の計算を同期で行う）。
- (void)setFinishingForTesting:(NSString*)spec {
    if (!_stacked) return;
    for (NSString* part in [spec componentsSeparatedByString:@","]) {
        NSArray* kv = [part componentsSeparatedByString:@"="];
        if ([kv count] != 2) continue;
        NSString* key = kv[0];
        NSString* value = kv[1];
        if ([key isEqualToString:@"channel"] && _stacked->channels() == 3) {
            [self applyChannelOffsets:stackcore::estimate_channel_offsets(*_stacked)];
        } else if ([key isEqualToString:@"wb"] && _stacked->channels() == 3) {
            stackcore::FrameBuffer aligned;
            stackcore::shift_channels(*_stacked, [self currentFinishingSettings].channels, aligned);
            double gains[3];
            stackcore::estimate_white_balance(aligned, gains);
            [self applyGainsRed:gains[0] blue:gains[2]];
        } else if ([key isEqualToString:@"crop"]) {
            // "center"（見えている画像の中央60%）または "x:y:w:h"（見えている画像の座標）。
            [self waitForFinishingForTesting];
            const int sw = [_preview imageWidth], sh = [_preview imageHeight];
            if ([value isEqualToString:@"center"]) {
                _cropRect = NSMakeRect(std::round(sw * 0.2), std::round(sh * 0.2), std::round(sw * 0.6), std::round(sh * 0.6));
            } else {
                NSArray* v = [value componentsSeparatedByString:@":"];
                if ([v count] == 4) {
                    _cropRect = NSMakeRect([v[0] doubleValue], [v[1] doubleValue], [v[2] doubleValue], [v[3] doubleValue]);
                }
            }
            [self applyCrop:nil];
        } else if ([key isEqualToString:@"rotate"]) {
            _rotationTurns = (([value intValue] % 4) + 4) % 4;
            [self finishingChanged:nil];
        } else if ([key isEqualToString:@"levels"]) {
            // "黒:中間:白"（黒・白は 0〜255）を RGB 全体に。
            NSArray* v = [value componentsSeparatedByString:@":"];
            if ([v count] == 3) {
                _levels[0].black = [v[0] doubleValue] / 255.0;
                _levels[0].gamma = [v[1] doubleValue];
                _levels[0].white = [v[2] doubleValue] / 255.0;
                [self finishingChanged:nil];
            }
        } else if ([key isEqualToString:@"dering"]) {
            [_deringSlider setDoubleValue:[value doubleValue]];
            [self finishingChanged:nil];
        } else if ([key isEqualToString:@"saturation"]) {
            [_saturationSlider setDoubleValue:[value doubleValue]];
            [self finishingChanged:nil];
        } else if ([key isEqualToString:@"linked"]) {
            [_linkedCheck setState:NSControlStateValueOn];
            [self waveletChanged:_linkedCheck];
            [_linkedSlider setDoubleValue:[value doubleValue]];
            [self waveletChanged:_linkedSlider];
        }
    }
    [self waitForFinishingForTesting];
}

// ---- 自己検証用のつまみ操作 ---------------------------------------------------

- (void)setCalibrationForTestingDark:(NSString*)dark flat:(NSString*)flat {
    [_darkPath release];
    _darkPath = [dark length] > 0 ? [dark copy] : nil;
    [_flatPath release];
    _flatPath = [flat length] > 0 ? [flat copy] : nil;
    [self invalidateCalibration];
}

- (void)showFrameAtSliderPositionForTesting:(int)position {
    [_viewModeSegment setSelectedSegment:0];
    [_frameSlider setDoubleValue:std::max(0.0, std::min([_frameSlider maxValue], static_cast<double>(position)))];
    [self frameSliderChanged:nil];
}

- (void)selectInspectorTabForTesting:(int)tab {
    [self selectInspectorTab:tab];
    // 詳細設定なども開いて見せる（配置の崩れを確かめるため）。
    // 利用者の開閉状態（NSUserDefaults）は書き換えず、画面の上だけで開く。
    for (NSString* key in _sections) {
        if ([self tabIndexForSectionKey:key] != tab) continue;
        [(NSButton*)_sectionHeaders[key] setHidden:NO];
        for (NSView* v in _sections[key]) [v setHidden:NO];
    }
}

- (void)setDrizzleIndexForTesting:(int)index {
    if (index < 0 || index >= kDrizzleChoiceCount) return;
    [_drizzleSegment setSelectedSegment:index];
    [self drizzleChanged:nil];
}

- (void)setZoomIndexForTesting:(int)index {
    if (index < 0 || index > 3) return;
    [_zoomControl setSelectedSegment:index];
    [self zoomChanged:nil];
}

- (void)setApHeatmapForTesting:(BOOL)on {
    [[_displayMenu itemAtIndex:2] setState:on ? NSControlStateValueOn : NSControlStateValueOff];
    [self apDisplayChanged:nil];
}

- (void)setSharpenForTesting:(double)value denoise:(double)denoise {
    for (int j = 0; j < kWaveletLayers; ++j) {
        [_sharpenSliders[j] setDoubleValue:(j < 3 ? value : 1.0)];
        [_denoiseSliders[j] setDoubleValue:denoise];
    }
    [self updateFinishingValueLabels];
    [self waitForFinishingForTesting];
}

// 切り抜きの枠を描いた状態にする（画面を撮るため。切り抜きはしない）。"x:y:w:h"
- (void)showCropBoxForTesting:(NSString*)spec {
    NSArray* v = [spec componentsSeparatedByString:@":"];
    if ([v count] != 4 || !_stacked) return;
    [_cropModeCheck setState:NSControlStateValueOn];
    [self cropModeChanged:nil];
    _cropRect = NSMakeRect([v[0] doubleValue], [v[1] doubleValue], [v[2] doubleValue], [v[3] doubleValue]);
    [self updateApOverlay];
    [self updateCropControls];
}

// 設定パネルを、指定したセクションの見出しが上に来るまでスクロールする（画面を撮るため）。
- (void)scrollToSectionForTesting:(NSString*)key {
    NSView* header = _sectionHeaders[key];
    if (!header) return;
    [[_inspectorScroll window] layoutIfNeeded];
    NSClipView* clip = [_inspectorScroll contentView];
    const NSRect r = [header convertRect:[header bounds] toView:[_inspectorScroll documentView]];
    [clip scrollToPoint:NSMakePoint(0, std::max<CGFloat>(0.0, NSMinY(r) - 4.0))];
    [_inspectorScroll reflectScrolledClipView:clip];
}

- (void)setWaveletPreviewForTesting:(BOOL)on {
    [_waveletPreviewCheck setState:on ? NSControlStateValueOn : NSControlStateValueOff];
    [self waveletPreviewChanged:nil];
}

@end
