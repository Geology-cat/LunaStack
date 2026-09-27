#import "MainWindowController_Private.h"

#include <algorithm>
#include <cmath>

namespace {

NSString* RejectReasonText(stackcore::RejectReason reason) {
    switch (reason) {
        case stackcore::RejectReason::LowCorrelation:
            return LSLocalizedString(@"参照と似ていない（追跡失敗・雲・欠落）");
        case stackcore::RejectReason::ShiftTooLarge:
            return LSLocalizedString(@"位置ずれが大きすぎる（視野外へ流れた）");
        case stackcore::RejectReason::StructuralOutlier:
            return LSLocalizedString(@"他のフレームと比べて崩れている");
        case stackcore::RejectReason::None:
        default:
            return LSLocalizedString(@"除外");
    }
}

}  // namespace

@implementation MainWindowController (Preview)

// ---- フレームの表示 -----------------------------------------------------------

// スライダーの位置が指すフレーム（ソース上の番号）。
// 品質順のときは、品質の高い順に並べた位置として読む。
- (int)currentFrameIndex {
    const int pos = static_cast<int>([_frameSlider doubleValue] + 0.5);
    if (_frameOrderByQuality && !_qualityOrder.empty()) {
        const int clamped = std::max(0, std::min(pos, static_cast<int>(_qualityOrder.size()) - 1));
        return _qualityOrder[static_cast<std::size_t>(clamped)];
    }
    return std::max(0, std::min(pos, std::max(0, _sourceFrames - 1)));
}

// フレーム index をいまの並びでの位置に直す。
- (int)sliderPositionForFrame:(int)index {
    if (_frameOrderByQuality && !_qualityOrder.empty()) {
        for (std::size_t k = 0; k < _qualityOrder.size(); ++k) {
            if (_qualityOrder[k] == index) return static_cast<int>(k);
        }
        return 0;
    }
    return index;
}

// 入力の1枚を表示する。プレビュー用に開いた入力を使い回す（P1）。
- (void)showSourceFrame:(int)index {
    if (!_previewSource) return;
    try {
        if (index < 0) index = 0;
        if (index >= _previewSource->frame_count()) index = _previewSource->frame_count() - 1;
        stackcore::FrameBuffer cfa, rgb;
        const bool raw = [_rawCfaCheck state] == NSControlStateValueOn;
        const stackcore::FrameBuffer* frame =
            stackcore::read_prepared_frame(*_previewSource, index, raw, cfa, rgb);
        _sourceChannels = frame->channels();
        [_preview clearFixedStretch];  // 入力のフレームは1枚ごとに明るさを合わせて見せる
        [_preview showFrameBuffer:*frame];
    } catch (const std::exception& e) {
        [_preview clearImage];
        [_statusLabel setStringValue:[NSString stringWithUTF8String:e.what()]];
    }
    [_graph setCurrentFrame:index];
    [self updateApOverlay];
    [self updateFrameInfoLabel];
}

// 「#1234 / 4617 · 上位 3.2% · 品質 0.0123」（元のファイルでの番号で出す）。
- (void)updateFrameInfoLabel {
    if (!_previewSource || _sourceFrames <= 0) {
        [_frameInfoLabel setStringValue:@""];
        return;
    }
    if ([_viewModeSegment selectedSegment] != 0) {
        [_frameInfoLabel setStringValue:[_viewModeSegment selectedSegment] == 1
                                            ? LSLocalizedString(@"参照画像")
                                            : LSLocalizedString(@"スタック結果")];
        return;
    }
    const int index = [self currentFrameIndex];
    const int original = _previewSource->original_index(index) + 1;
    // 狭いウインドウでは末尾から切り詰まるので、いちばん知りたい「上位何%」を先頭に置く。
    NSString* number = [NSString stringWithFormat:@"#%d / %d", original,
                                                  std::max(_sourceTotalFrames, _sourceFrames)];
    NSString* text = number;
    if (index < static_cast<int>(_frameInfos.size())) {
        const stackcore::FrameInfo& info = _frameInfos[static_cast<std::size_t>(index)];
        if (!info.accepted) {
            text = [NSString stringWithFormat:@"%@ · %@", RejectReasonText(info.reason), number];
        } else {
            int accepted = 0, rank = 0;
            for (std::size_t k = 0; k < _qualityOrder.size(); ++k) {
                if (_frameInfos[static_cast<std::size_t>(_qualityOrder[k])].accepted) ++accepted;
                if (_qualityOrder[k] == index) rank = accepted;
            }
            if (accepted > 0 && rank > 0) {
                text = [NSString stringWithFormat:LSLocalizedString(@"上位 %.1f%% · %@ · 品質 %.4g"),
                                                  100.0 * rank / accepted, number, info.quality];
            }
        }
    }
    [_frameInfoLabel setStringValue:text];
    [_frameInfoLabel setToolTip:text];
    [_frameSlider setToolTip:text];
}

- (void)frameSliderChanged:(id)sender {
    (void)sender;
    [_viewModeSegment setSelectedSegment:0];
    [self showSourceFrame:[self currentFrameIndex]];
}

// スライダーの並びを切り替える。いま見ているフレームはそのまま保つ。
- (void)setFrameOrderByQuality:(BOOL)byQuality {
    const int current = [self currentFrameIndex];
    _frameOrderByQuality = byQuality && !_qualityOrder.empty();
    [_frameOrderSegment setSelectedSegment:_frameOrderByQuality ? 1 : 0];
    [_graphMode setSelectedSegment:_frameOrderByQuality ? 1 : 0];
    [_graph setSortedByQuality:_frameOrderByQuality];
    [self updateSliderRange];
    [_frameSlider setDoubleValue:[self sliderPositionForFrame:current]];
    [self updateFrameInfoLabel];
}

- (void)frameOrderChanged:(id)sender {
    (void)sender;
    if ([_frameOrderSegment selectedSegment] == 1 && _qualityOrder.empty()) {
        [_frameOrderSegment setSelectedSegment:0];
        [_statusLabel setStringValue:LSLocalizedString(@"品質順に並べるには、先に品質評価をしてください")];
        return;
    }
    [self setFrameOrderByQuality:[_frameOrderSegment selectedSegment] == 1];
}

// グラフの並びとスライダーの並びは同じものにする（左端＝スライダーの左端）。
- (void)graphModeChanged:(id)sender {
    (void)sender;
    if ([_graphMode selectedSegment] == 1 && _qualityOrder.empty()) {
        [_graphMode setSelectedSegment:0];
        return;
    }
    [self setFrameOrderByQuality:[_graphMode selectedSegment] == 1];
}

- (void)qualityGraphView:(QualityGraphView*)view didSelectFrame:(int)index {
    (void)view;
    if (index < 0 || index >= _sourceFrames) return;
    [_viewModeSegment setSelectedSegment:0];
    [_frameSlider setDoubleValue:[self sliderPositionForFrame:index]];
    [self showSourceFrame:index];
}

- (void)previewView:(PreviewView*)view didRequestFrameStep:(int)step {
    (void)view;
    [self stepFrameBy:step];
}

// ←→ のボタン（tag が -1 / +1）。
- (void)frameStepButton:(id)sender {
    [self stepFrameBy:static_cast<int>([sender tag])];
}

// スライダーの並び（時系列・品質順）で step 枚ぶん送る。
- (void)stepFrameBy:(int)step {
    if (_sourceFrames <= 0) return;
    if ([_viewModeSegment selectedSegment] != 0) [_viewModeSegment setSelectedSegment:0];
    const int pos = static_cast<int>([_frameSlider doubleValue] + 0.5) + step;
    [_frameSlider setDoubleValue:std::max(0.0, std::min([_frameSlider maxValue], static_cast<double>(pos)))];
    [self showSourceFrame:[self currentFrameIndex]];
}

- (void)previewView:(PreviewView*)view hoverDescription:(NSString*)text {
    (void)view;
    [_pixelLabel setStringValue:text ? text : @""];
}

- (void)previewViewZoomDidChange:(PreviewView*)view {
    (void)view;
    [self syncZoomControl];
}

// 表示モードの切替（UI設計書 §5.1）。
- (void)viewModeChanged:(id)sender {
    (void)sender;
    switch ([_viewModeSegment selectedSegment]) {
        case 1:
            if (_referenceImage) {
                [_preview clearFixedStretch];
                [_preview showSharedFrame:_referenceImage];
            } else {
                [_statusLabel
                    setStringValue:LSLocalizedString(@"参照画像はまだありません（アライメントすると作られます）")];
                [_viewModeSegment setSelectedSegment:0];
                [self showSourceFrame:[self currentFrameIndex]];
            }
            break;
        case 2:
            if (_stacked) {
                [self showFinishedOrStacked];
            } else {
                [_statusLabel setStringValue:LSLocalizedString(@"スタック結果はまだありません")];
                [_viewModeSegment setSelectedSegment:0];
                [self showSourceFrame:[self currentFrameIndex]];
            }
            break;
        case 0:
        default:
            [self showSourceFrame:[self currentFrameIndex]];
            break;
    }
    [self updateApOverlay];
    [self updateFrameInfoLabel];
}

// ---- ズーム -----------------------------------------------------------------

- (void)zoomChanged:(id)sender {
    (void)sender;
    const double zooms[] = {0.0, 1.0, 2.0, 4.0};
    const NSInteger i = [_zoomControl selectedSegment];
    if (i >= 0 && i < 4) [_preview setZoom:zooms[i]];
}

// ホイール等で倍率が変わったら、ツールバーの表示を合わせる（一致する段が無ければ選択なし）。
- (void)syncZoomControl {
    const double z = [_preview zoom];
    NSInteger segment = -1;
    if (z <= 0.0) segment = 0;
    else if (std::fabs(z - 1.0) < 1e-3) segment = 1;
    else if (std::fabs(z - 2.0) < 1e-3) segment = 2;
    else if (std::fabs(z - 4.0) < 1e-3) segment = 3;
    [_zoomControl setSelectedSegment:segment];
    if (segment < 0) {
        [_pixelLabel setStringValue:[NSString stringWithFormat:LSLocalizedString(@"倍率 %.0f%%"), z * 100.0]];
    }
}

- (void)zoomIn:(id)sender {
    (void)sender;
    [_preview zoomInStep];
    [self syncZoomControl];
}

- (void)zoomOut:(id)sender {
    (void)sender;
    [_preview zoomOutStep];
    [self syncZoomControl];
}

- (void)zoomToFit:(id)sender {
    (void)sender;
    [_preview setZoom:0.0];
    [self syncZoomControl];
}

- (void)zoomActualPixels:(id)sender {
    (void)sender;
    [_preview setZoom:1.0];
    [self syncZoomControl];
}

// ---- 表示の切り替え -----------------------------------------------------------

- (void)displayMenuChanged:(id)sender {
    NSMenuItem* item = (NSMenuItem*)sender;
    [item setState:[item state] == NSControlStateValueOn ? NSControlStateValueOff
                                                         : NSControlStateValueOn];
    [self apDisplayChanged:nil];
    [_preview setDisplayStretch:[[_displayMenu itemAtIndex:3] state] == NSControlStateValueOn];
}

- (void)stretchToggled:(id)sender {
    (void)sender;
    [_preview setDisplayStretch:[[_displayMenu itemAtIndex:3] state] == NSControlStateValueOn];
}

- (void)apDisplayChanged:(id)sender {
    (void)sender;
    [_preview setShowAlignmentPoints:[[_displayMenu itemAtIndex:1] state] == NSControlStateValueOn];
    [_preview setApHeatmap:[[_displayMenu itemAtIndex:2] state] == NSControlStateValueOn];
    [_preview setApEditing:[_apEditCheck state] == NSControlStateValueOn];
    if ([_apEditCheck state] == NSControlStateValueOn) {
        [[self window] makeFirstResponder:_preview];
    }
}

// スタック結果がDrizzleで何倍になっているか（スタックしたときの倍率。いまのつまみではない）。
- (double)stackedScale {
    NSNumber* scale = _stackedInfo[@"drizzle"];
    return scale ? [scale doubleValue] : 1.0;
}

// AP枠の倍率は「いま表示している画像」で決める。
// フレーム・参照画像は入力と同じ大きさ、スタック結果だけがDrizzle倍率ぶん大きい。
- (double)overlayScale {
    if ([_viewModeSegment selectedSegment] == 2 && _stacked) return [self stackedScale];
    return 1.0;
}

- (void)updateApOverlay {
    // 仕上げで回転・反転した結果や、切り抜いたスタック結果の上では、AP枠の位置が合わないので描かない。
    const BOOL onResult = [_viewModeSegment selectedSegment] == 2 && _stacked;
    const BOOL transformed = onResult && (_uncroppedStacked ||
                                          ([_waveletPreviewCheck state] == NSControlStateValueOn &&
                                           ![self currentFinishingSettings].geometry.identity()));
    // 切り抜きの枠は、枠を描いている間だけスタック結果の上に見せる。
    // 処理範囲は、フレーム（入力そのままの画像）の上にいつも見せ、描くモードの間は編集できる。
    const BOOL showCropRect = onResult && [_cropModeCheck state] == NSControlStateValueOn;
    const BOOL onFrames = [_viewModeSegment selectedSegment] == 0 && _previewSource;
    const BOOL roiEditing = onFrames && [_roiModeCheck state] == NSControlStateValueOn;
    if (showCropRect) {
        [_preview setCropEditing:YES];
        [_preview setCropOverlay:_cropRect];
    } else if (onFrames && (roiEditing || _roiRect.size.width > 0)) {
        [_preview setCropEditing:roiEditing];
        [_preview setCropOverlay:_roiRect];
    } else {
        [_preview setCropEditing:NO];
        [_preview setCropOverlay:NSZeroRect];
    }
    if (transformed) {
        [_preview clearAlignmentPoints];
        return;
    }
    const double scale = [self overlayScale];
    // 手動配置をまだ実行していない（解析結果と一致しない）ときは、その配置を描く。
    if (_manualPointsActive && ![self analysisUsable]) {
        int size = LSApSizeAt([_apSizePopup indexOfSelectedItem]);
        if (size == 0) size = _analysis ? _analysis->ap_size : 64;  // 自動のときは表示だけ暫定値で描く
        const std::vector<double> none;
        std::vector<stackcore::AlignmentPoint> shown = _manualPoints;
        if ([_viewModeSegment selectedSegment] == 0 && _roiRect.size.width > 0) {
            for (stackcore::AlignmentPoint& p : shown) {
                p.cx += static_cast<int>(_roiRect.origin.x);
                p.cy += static_cast<int>(_roiRect.origin.y);
            }
        }
        [_preview setAlignmentPoints:shown apSize:size meanQualities:none coordinateScale:scale];
        [_apCountLabel setStringValue:[NSString stringWithFormat:
                                                    LSLocalizedString(@"手動の位置合わせ領域 %zu個（未実行）"),
                                                    _manualPoints.size()]];
        return;
    }
    if (!_analysis || _analysis->points.empty()) {
        [_preview clearAlignmentPoints];
        [_apCountLabel setStringValue:@""];
        return;
    }
    const std::vector<double> quality = ap_mean_quality(*_analysis);
    if ([_viewModeSegment selectedSegment] == 0 && _roiRect.size.width > 0) {
        // 解析は処理範囲の座標。フレーム表示は入力全体なので、範囲の左上だけずらして描く。
        // 範囲を変えたあと（解析が今の範囲のものでない）は描かない。
        if (![self analysisUsable]) {
            [_preview clearAlignmentPoints];
            return;
        }
        std::vector<stackcore::AlignmentPoint> shifted = _analysis->points;
        for (stackcore::AlignmentPoint& p : shifted) {
            p.cx += static_cast<int>(_roiRect.origin.x);
            p.cy += static_cast<int>(_roiRect.origin.y);
        }
        [_preview setAlignmentPoints:shifted apSize:_analysis->ap_size meanQualities:quality coordinateScale:scale];
        [_apCountLabel setStringValue:[NSString stringWithFormat:LSLocalizedString(@"位置合わせ領域 %zu個 / %d px"),
                                                                 _analysis->points.size(), _analysis->ap_size]];
        return;
    }
    [_preview setAlignmentPoints:_analysis->points
                          apSize:_analysis->ap_size
                   meanQualities:quality
                 coordinateScale:scale];
    [_apCountLabel setStringValue:[NSString stringWithFormat:LSLocalizedString(@"位置合わせ領域 %zu個 / %d px"),
                                                             _analysis->points.size(),
                                                             _analysis->ap_size]];
}

// サイドカーに入っている参照画像を FrameBuffer に戻す。
// 「解析で何を基準にしたか」は、結果が変なときに真っ先に見たいものである。
- (void)rebuildReferenceImage {
    _referenceImage.reset();
    if (!_analysis) return;
    const int w = _analysis->width, h = _analysis->height, c = _analysis->channels;
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    if (w <= 0 || h <= 0 || c <= 0 ||
        _analysis->reference.size() < pixels * static_cast<std::size_t>(c)) {
        return;
    }
    auto image = std::make_shared<stackcore::FrameBuffer>();
    image->reset(w, h, c);
    for (int ch = 0; ch < c; ++ch) {
        for (int y = 0; y < h; ++y) {
            const float* src = _analysis->reference.data() +
                               static_cast<std::size_t>(ch) * pixels +
                               static_cast<std::size_t>(y) * w;
            float* dst = image->row(ch, y);
            for (int x = 0; x < w; ++x) dst[x] = src[x];
        }
    }
    image->invalidate_luma();
    _referenceImage = image;
}

// ---- 品質グラフとスライダーの並び ---------------------------------------------

- (void)showFrames:(const std::vector<stackcore::FrameInfo>&)frames {
    _frameInfos = frames;
    if (frames.empty()) {
        [_graph clearData];
        _qualityOrder.clear();
        return;
    }
    std::vector<double> q(frames.size());
    std::vector<unsigned char> ok(frames.size());
    int rejected = 0;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        q[i] = frames[i].quality;
        ok[i] = frames[i].accepted ? 1 : 0;
        if (!frames[i].accepted) ++rejected;
    }
    [_graph setQualities:q.data() accepted:ok.data() count:static_cast<int>(frames.size())];
    [_graph setCutPercent:[_topSlider doubleValue]];
    const int current = [self currentFrameIndex];
    [self rebuildQualityOrder];
    if (_frameOrderByQuality) [_frameSlider setDoubleValue:[self sliderPositionForFrame:current]];

    _rejectedFrames = rejected;
    [self updateBanner];
    [self updateFrameInfoLabel];
}

// 品質順の並び。エンジンの select_top_frames と同じ規則（採用フレームを品質降順、
// 同点はフレーム番号昇順）にし、除外フレームは最後に番号順で置く。
- (void)rebuildQualityOrder {
    _qualityOrder.clear();
    const std::vector<stackcore::FrameInfo>& f = _frameInfos;
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (f[i].accepted) _qualityOrder.push_back(static_cast<int>(i));
    }
    std::sort(_qualityOrder.begin(), _qualityOrder.end(), [&f](int a, int b) {
        const double qa = f[static_cast<std::size_t>(a)].quality;
        const double qb = f[static_cast<std::size_t>(b)].quality;
        if (qa != qb) return qa > qb;
        return a < b;
    });
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (!f[i].accepted) _qualityOrder.push_back(static_cast<int>(i));
    }
    // 入力より多ければ（範囲を狭めた直後の古い結果など）並びを使わない。
    // 少ないのは先頭Nフレームだけ評価した場合で、そのときは評価した分だけを並べる。
    if (static_cast<int>(_qualityOrder.size()) > _sourceFrames) _qualityOrder.clear();
    if (_frameOrderByQuality) [self updateSliderRange];
}

// スライダーの範囲。品質順では評価したフレームだけを並べる。
- (void)updateSliderRange {
    const int count = (_frameOrderByQuality && !_qualityOrder.empty())
                          ? static_cast<int>(_qualityOrder.size())
                          : _sourceFrames;
    [_frameSlider setMaxValue:std::max(0, count - 1)];
}

// ---- 警告バナー（UI設計書 §7.2） --------------------------------------------------

// CLIの `info` が出す診断と同じことを、GUIでも見逃さないようにする。
// **黙って処理しない**のが趣旨なので、原因と対処先を1行にまとめる。
- (void)updateBanner {
    NSArray* dismissed = [[NSUserDefaults standardUserDefaults] arrayForKey:@"dismissedByteOrderBanners"];
    const BOOL byteOrderDismissed = [dismissed containsObject:[self inputPathString]];
    if (_bannerDismissed) {
        [_bannerLabel setStringValue:@""];
        for (NSButton* b in @[ _bannerLittleButton, _bannerBigButton, _bannerDepthButton,
                               _bannerDetailsButton, _bannerCloseButton ]) {
            [b setHidden:YES];
        }
        return;
    }
    const BOOL showByteOrder = _byteOrderSuspect && !byteOrderDismissed &&
                               [_endianPopup indexOfSelectedItem] == 0;
    NSMutableArray* parts = [NSMutableArray array];
    if (showByteOrder) {
        // 自動判定がヘッダと違うこと自体は異常ではない（ソフト間でフラットの解釈が割れている）。
        [parts addObject:LSLocalizedString(
                             @"ヘッダと異なるバイトオーダーで読んでいます（自動判定）。画像が正常なら問題ありません")];
    }
    if (_looksLikeShallowDepth && [_depthPopup indexOfSelectedItem] == 0) {
        [parts addObject:LSLocalizedString(
                             @"16bitですが実測は12bit幅です。暗く写るなら品質評価タブで「12bitとして扱う」を選んでください")];
    }
    if (_rejectedFrames > 0) {
        [parts addObject:[NSString
                             stringWithFormat:
                                 LSLocalizedString(@"%d 枚を自動除外しました（視野外・追跡失敗）"),
                                                    _rejectedFrames]];
    }
    [_bannerLabel setStringValue:[parts componentsJoinedByString:@" ／ "]];
    [_bannerLabel setTextColor:(_looksLikeShallowDepth || _rejectedFrames > 0)
                                   ? [NSColor systemOrangeColor]
                                   : [NSColor secondaryLabelColor]];
    [_bannerLittleButton setHidden:!showByteOrder];
    [_bannerBigButton setHidden:!showByteOrder];
    [_bannerDepthButton setHidden:!(_looksLikeShallowDepth && [_depthPopup indexOfSelectedItem] == 0)];
    [_bannerDetailsButton setHidden:_rejectedFrames == 0];
    [_bannerCloseButton setHidden:[parts count] == 0];
}

- (void)useLittleEndianFromBanner:(id)sender {
    (void)sender;
    [_endianPopup selectItemAtIndex:1];
    [self inputInterpretationChanged:nil];
}

- (void)useBigEndianFromBanner:(id)sender {
    (void)sender;
    [_endianPopup selectItemAtIndex:2];
    [self inputInterpretationChanged:nil];
}

- (void)use12BitFromBanner:(id)sender {
    (void)sender;
    [_depthPopup selectItemAtIndex:1];
    [self inputInterpretationChanged:nil];
}

// 閉じた警告は、バイトオーダーの注意に限りそのファイルでは以後出さない
// （毎回同じ注意が出ると、本当に大事な警告が目に入らなくなる）。
- (void)dismissBanner:(id)sender {
    (void)sender;
    _bannerDismissed = YES;
    if (_byteOrderSuspect && !_inputPath.empty()) {
        NSMutableArray* list = [[[[NSUserDefaults standardUserDefaults]
            arrayForKey:@"dismissedByteOrderBanners"] mutableCopy] autorelease];
        if (!list) list = [NSMutableArray array];
        if (![list containsObject:[self inputPathString]]) [list addObject:[self inputPathString]];
        while ([list count] > 200) [list removeObjectAtIndex:0];
        [[NSUserDefaults standardUserDefaults] setObject:list forKey:@"dismissedByteOrderBanners"];
    }
    [self updateBanner];
}

// 除外したフレームの一覧（UI設計書 §7.2 の［詳細を見る］）。
- (void)showRejectedFrames:(id)sender {
    (void)sender;
    NSMutableString* text = [NSMutableString string];
    int first = -1;
    for (std::size_t i = 0; i < _frameInfos.size(); ++i) {
        const stackcore::FrameInfo& f = _frameInfos[i];
        if (f.accepted) continue;
        if (first < 0) first = static_cast<int>(i);
        const int original = _previewSource ? _previewSource->original_index(static_cast<int>(i)) + 1
                                            : static_cast<int>(i) + 1;
        [text appendFormat:LSLocalizedString(@"#%d  %@（類似度 %.3f・位置ずれ %d,%d）\n"), original,
                           RejectReasonText(f.reason), f.similarity, f.dx, f.dy];
    }
    if (first < 0) return;
    NSAlert* alert = [[[NSAlert alloc] init] autorelease];
    [alert setMessageText:[NSString stringWithFormat:LSLocalizedString(@"除外したフレーム（%d 枚）"),
                                                     _rejectedFrames]];
    [alert setInformativeText:LSLocalizedString(@"除外したフレームは、どの工程でも使いません。")];
    NSScrollView* scroll = [[[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 420, 220)] autorelease];
    [scroll setHasVerticalScroller:YES];
    [scroll setBorderType:NSBezelBorder];
    NSTextView* view = [[[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 220)] autorelease];
    [view setEditable:NO];
    [view setFont:[NSFont monospacedDigitSystemFontOfSize:11.0 weight:NSFontWeightRegular]];
    [view setString:text];
    [scroll setDocumentView:view];
    [alert setAccessoryView:scroll];
    [alert addButtonWithTitle:LSLocalizedString(@"閉じる")];
    [alert addButtonWithTitle:LSLocalizedString(@"最初の除外フレームを表示")];
    if ([alert runModal] == NSAlertSecondButtonReturn) {
        [self qualityGraphView:_graph didSelectFrame:first];
    }
}

// ---- APの手動編集（取り消し可能） ------------------------------------------------

- (NSData*)manualPointsSnapshot {
    NSMutableData* data = [NSMutableData data];
    const int active = _manualPointsActive ? 1 : 0;
    [data appendBytes:&active length:sizeof(active)];
    for (const stackcore::AlignmentPoint& p : _manualPoints) {
        const int xy[2] = {p.cx, p.cy};
        [data appendBytes:xy length:sizeof(xy)];
    }
    return data;
}

// 取り消しの登録。変更の直前の状態を覚える。
- (void)registerManualPointsUndo:(NSString*)actionName {
    NSUndoManager* undo = [[self window] undoManager];
    [undo registerUndoWithTarget:self
                        selector:@selector(setManualPointsForUndo:)
                          object:[self manualPointsSnapshot]];
    [undo setActionName:LSLocalizedString(actionName)];
}

- (void)setManualPointsForUndo:(NSData*)data {
    // やり直し（redo）のために、戻す前の状態を登録しておく。
    NSUndoManager* undo = [[self window] undoManager];
    [undo registerUndoWithTarget:self
                        selector:@selector(setManualPointsForUndo:)
                          object:[self manualPointsSnapshot]];
    const std::size_t count = ([data length] - sizeof(int)) / (2 * sizeof(int));
    const int* raw = static_cast<const int*>([data bytes]);
    _manualPointsActive = raw[0] != 0;
    _manualPoints.clear();
    for (std::size_t i = 0; i < count; ++i) {
        stackcore::AlignmentPoint p;
        p.cx = raw[1 + i * 2];
        p.cy = raw[2 + i * 2];
        _manualPoints.push_back(p);
    }
    [self applyManualPoints];
}

- (void)previewView:(PreviewView*)view didAddApAtX:(int)x y:(int)y {
    (void)view;
    [self registerManualPointsUndo:@"位置合わせ領域の追加"];
    [self ensureManualPointsInitialized];
    stackcore::AlignmentPoint p;
    p.cx = x;
    p.cy = y;
    if ([_viewModeSegment selectedSegment] == 0 && _roiRect.size.width > 0) {
        // フレーム表示（入力全体）の上で置いたときは、処理範囲の座標に直す。
        p.cx -= static_cast<int>(_roiRect.origin.x);
        p.cy -= static_cast<int>(_roiRect.origin.y);
    }
    _manualPoints.push_back(p);
    [self applyManualPoints];
}

- (void)previewView:(PreviewView*)view didDeleteApAtIndex:(NSInteger)index {
    (void)view;
    [self registerManualPointsUndo:@"位置合わせ領域の削除"];
    [self ensureManualPointsInitialized];
    if (index < 0 || index >= static_cast<NSInteger>(_manualPoints.size())) return;
    _manualPoints.erase(_manualPoints.begin() + index);
    [self applyManualPoints];
}

// 自動配置の結果を手動リストの初期値にする。
// 空のまま1点追加すると「AP1個だけ」になり、その挙動は誰も望んでいない。
- (void)ensureManualPointsInitialized {
    if (_manualPointsActive) return;
    _manualPointsActive = YES;
    if (_analysis && !_analysis->points.empty()) _manualPoints = _analysis->points;
}

- (void)applyManualPoints {
    // 位置合わせ領域が変わったら、アライメント以降はもう使えない
    // （指紋に手動配置が入っているので、次の判定で自動的に無効になる）。
    [self updateApOverlay];
    [self updateControlsEnabled];
}

- (void)resetApPlacement:(id)sender {
    (void)sender;
    [self registerManualPointsUndo:@"自動配置に戻す"];
    _manualPointsActive = NO;
    _manualPoints.clear();
    [self updateApOverlay];
    [self updateControlsEnabled];
}

- (void)clearApPlacement:(id)sender {
    (void)sender;
    // 「自動配置に戻す」とは別の操作。消したまま解析すれば
    // 「APが1つも置けませんでした」で止まる。それが正しい。
    // 消したのに黙って置き直されるほうが、よほど分かりにくい。
    [self registerManualPointsUndo:@"位置合わせ領域をすべて消去"];
    _manualPointsActive = YES;
    _manualPoints.clear();
    [_preview clearAlignmentPoints];
    [_apCountLabel setStringValue:LSLocalizedString(@"位置合わせ領域 0個（手動）")];
    [_statusLabel
        setStringValue:LSLocalizedString(@"位置合わせ領域をすべて消しました。プレビューをクリックして置き直せます")];
    [self updateControlsEnabled];
}

@end
