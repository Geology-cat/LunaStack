#import "MainWindowController_Private.h"

#include <algorithm>
#include <cmath>

#include "stackcore/map_pipeline.hpp"
#include "stackcore/video_source.hpp"
#include "stackcore/wavelet.hpp"

@implementation MainWindowController (Preview)

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

- (void)showFrames:(const std::vector<stackcore::FrameInfo>&)frames {
    if (frames.empty()) {
        [_graph clearData];
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

    _rejectedFrames = rejected;
    _bannerDismissed = NO;
    [self updateBanner];
}

// 警告バナー（UI設計書 §7.2）。
//
// CLIの `info` が出す診断と同じことを、GUIでも見逃さないようにする。
// **黙って処理しない**のが趣旨なので、原因と対処先を1行にまとめる。
- (void)updateBanner {
    if (_bannerDismissed) {
        [_bannerLabel setStringValue:@""];
        [_bannerLittleButton setHidden:YES];
        [_bannerBigButton setHidden:YES];
        [_bannerDepthButton setHidden:YES];
        [_bannerCloseButton setHidden:YES];
        return;
    }
    NSMutableArray* parts = [NSMutableArray array];
    if (_byteOrderSuspect) {
        [parts addObject:LSLocalizedString(
                             @"バイトオーダーがヘッダの主張と違います。画像が破綻して見えるなら品質評価タブで切り替えてください")];
    }
    if (_looksLikeShallowDepth) {
        [parts addObject:LSLocalizedString(
                             @"16bitですが実測は12bit幅です。暗く写るなら品質評タブで「12bitとして扱う」を選んでください")];
    }
    if (_rejectedFrames > 0) {
        [parts addObject:[NSString
                             stringWithFormat:
                                 LSLocalizedString(@"%d 枚を自動除外しました（視野外・追跡失敗）"),
                                                    _rejectedFrames]];
    }
    [_bannerLabel setStringValue:[parts componentsJoinedByString:@" ／ "]];
    [_bannerLittleButton setHidden:!_byteOrderSuspect];
    [_bannerBigButton setHidden:!_byteOrderSuspect];
    [_bannerDepthButton setHidden:!_looksLikeShallowDepth];
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

- (void)dismissBanner:(id)sender {
    (void)sender;
    _bannerDismissed = YES;
    [self updateBanner];
}

// 入力の1枚を表示する。
- (void)showSourceFrame:(int)index {
    if (_inputPath.empty()) return;
    try {
        const std::unique_ptr<stackcore::VideoSource> source =
            stackcore::open_video(_inputPath, [self currentOpenOptions]);
        if (index < 0) index = 0;
        if (index >= source->frame_count()) index = source->frame_count() - 1;
        stackcore::FrameBuffer cfa, rgb;
        const stackcore::FrameBuffer* frame =
            stackcore::read_prepared_frame(*source, index, false, cfa, rgb);
        _sourceChannels = frame->channels();
        [_preview showFrameBuffer:*frame];
    } catch (const std::exception&) {
        [_preview clearImage];
    }
}

- (void)frameSliderChanged:(id)sender {
    (void)sender;
    [_viewModeSegment setSelectedSegment:0];
    [self showSourceFrame:static_cast<int>([_frameSlider doubleValue] + 0.5)];
}

// 表示モードの切替（UI設計書 §5.1）。
- (void)viewModeChanged:(id)sender {
    (void)sender;
    switch ([_viewModeSegment selectedSegment]) {
        case 1:
            if (_referenceImage) {
                [_preview showFrameBuffer:*_referenceImage];
            } else {
                [_statusLabel
                    setStringValue:LSLocalizedString(@"参照画像はまだありません（解析すると作られます）")];
                [_viewModeSegment setSelectedSegment:0];
            }
            break;
        case 2:
            if (_displayed && _stacked) {
                const BOOL showEffect =
                    [_waveletPreviewCheck state] == NSControlStateValueOn;
                [_preview showFrameBuffer:showEffect ? *_displayed : *_stacked];
            } else {
                [_statusLabel setStringValue:LSLocalizedString(@"スタック結果はまだありません")];
                [_viewModeSegment setSelectedSegment:0];
            }
            break;
        case 0:
        default:
            [self showSourceFrame:static_cast<int>([_frameSlider doubleValue] + 0.5)];
            break;
    }
}

- (void)updateApOverlay {
    if (!_analysis || _analysis->points.empty()) {
        [_preview clearAlignmentPoints];
        [_apCountLabel setStringValue:_manualPoints.empty()
                                          ? @""
                                          : [NSString stringWithFormat:
                                                            LSLocalizedString(@"手動の位置合わせ領域 %zu個（未実行）"),
                                                                       _manualPoints.size()]];
        return;
    }
    const double scale = _stacked ? LSDrizzleScaleAt([_drizzleSegment selectedSegment]) : 1.0;
    const std::vector<double> quality = ap_mean_quality(*_analysis);
    [_preview setAlignmentPoints:_analysis->points
                          apSize:_analysis->ap_size
                   meanQualities:quality
                 coordinateScale:scale];
    [_apCountLabel setStringValue:[NSString stringWithFormat:LSLocalizedString(@"位置合わせ領域 %zu個 / %d px"),
                                                             _analysis->points.size(),
                                                             _analysis->ap_size]];
}

// ---- APの手動編集 ---------------------------------------------------------

- (void)previewView:(PreviewView*)view didAddApAtX:(int)x y:(int)y {
    (void)view;
    [self ensureManualPointsInitialized];
    stackcore::AlignmentPoint p;
    p.cx = x;
    p.cy = y;
    _manualPoints.push_back(p);
    [self applyManualPoints];
}

- (void)previewView:(PreviewView*)view didDeleteApAtIndex:(NSInteger)index {
    (void)view;
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
    // 位置合わせ領域が変わったら、アライメント以降はもう使えない。
    _globalStage.reset();
    [_globalSignature release];
    _globalSignature = nil;
    _analysis.reset();
    [_analysisSignature release];
    _analysisSignature = nil;

    const std::vector<double> none;
    int size = LSApSizeAt([_apSizePopup indexOfSelectedItem]);
    if (size == 0) size = 64;  // 自動のときは表示だけ暫定値で描く
    [_preview setAlignmentPoints:_manualPoints apSize:size meanQualities:none coordinateScale:1.0];
    [_apCountLabel setStringValue:[NSString stringWithFormat:
                                                        LSLocalizedString(@"手動の位置合わせ領域 %zu個（未実行）"),
                                                             _manualPoints.size()]];
    [self updateControlsEnabled];
}

- (void)resetApPlacement:(id)sender {
    (void)sender;
    _manualPointsActive = NO;
    _manualPoints.clear();
    _globalStage.reset();
    [_globalSignature release];
    _globalSignature = nil;
    _analysis.reset();
    [_analysisSignature release];
    _analysisSignature = nil;
    [_preview clearAlignmentPoints];
    [_apCountLabel setStringValue:@""];
    [self updateControlsEnabled];
}

- (void)clearApPlacement:(id)sender {
    (void)sender;
    // 「自動配置に戻す」とは別の操作。消したまま解析すれば
    // 「APが1つも置けませんでした」で止まる。それが正しい。
    // 消したのに黙って置き直されるほうが、よほど分かりにくい。
    _manualPointsActive = YES;
    _manualPoints.clear();
    _globalStage.reset();
    [_globalSignature release];
    _globalSignature = nil;
    _analysis.reset();
    [_analysisSignature release];
    _analysisSignature = nil;
    [_preview clearAlignmentPoints];
    [_apCountLabel setStringValue:LSLocalizedString(@"位置合わせ領域 0個（手動）")];
    [_statusLabel
        setStringValue:LSLocalizedString(@"位置合わせ領域をすべて消しました。プレビューをクリックして置き直せます")];
    [self updateControlsEnabled];
}

@end
