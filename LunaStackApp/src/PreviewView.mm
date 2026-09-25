#import "PreviewView.h"

#import "Localization.h"

#include <algorithm>
#include <cmath>
#include <vector>

// 定義より前に出てくるメソッドの宣言。無いと型が分からず素通りしてしまう。
@interface PreviewView ()
- (NSPoint)clampedPan:(NSPoint)pan;
- (double)effectiveScale;
- (double)backingScale;
- (void)rebuildImage;
- (void)releaseMipmaps;
@end

namespace {

// 縮小表示用の段数（1/2, 1/4, 1/8, 1/16）。
constexpr int kMipLevels = 4;
// ホイール・ピンチで使える倍率の範囲（画面の実画素に対して）。
constexpr double kMinZoom = 0.05;
constexpr double kMaxZoom = 32.0;

}  // namespace

@implementation PreviewView {
    CGImageRef _image;
    CGImageRef _mips[kMipLevels];
    int _imageWidth;
    int _imageHeight;
    // 直近に渡された画像。ストレッチの切り替えと画素値の表示で使う。
    std::shared_ptr<const stackcore::FrameBuffer> _source;

    // APオーバーレイ。
    std::vector<stackcore::AlignmentPoint> _points;
    std::vector<double> _apQuality;  // 正規化済み 0..1。空ならヒートマップなし
    int _apSize;
    double _apScale;
    NSInteger _selectedAp;

    // パン（表示位置のずらし量、ビュー座標）。
    NSPoint _pan;
    BOOL _panning;
    NSPoint _panStart;
    NSPoint _panOrigin;

    NSTrackingArea* _tracking;

    BOOL _fixedStretch;
    float _fixedLo;
    float _fixedHi;
    float _fixedGamma;
}

@synthesize zoom = _zoom;
@synthesize displayStretch = _displayStretch;
@synthesize showAlignmentPoints = _showAlignmentPoints;
@synthesize apHeatmap = _apHeatmap;
@synthesize apEditing = _apEditing;
@synthesize cropOverlay = _cropOverlay;
@synthesize delegate = _delegate;

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        _image = NULL;
        for (int i = 0; i < kMipLevels; ++i) _mips[i] = NULL;
        _imageWidth = 0;
        _imageHeight = 0;
        _zoom = 0.0;
        _displayStretch = YES;
        _apSize = 0;
        _apScale = 1.0;
        _selectedAp = -1;
        _showAlignmentPoints = YES;
        _apHeatmap = NO;
        _apEditing = NO;
        _pan = NSZeroPoint;
        _panning = NO;
        _cropOverlay = NSZeroRect;
        _tracking = nil;
        _fixedStretch = NO;
        _fixedLo = 0.0f;
        _fixedHi = 1.0f;
        _fixedGamma = 0.75f;
    }
    return self;
}

- (void)dealloc {
    if (_image) CGImageRelease(_image);
    [self releaseMipmaps];
    [_tracking release];
    [super dealloc];
}

- (BOOL)isOpaque {
    return YES;
}

- (BOOL)acceptsFirstResponder {
    // Delete キーでAPを消し、←→でフレームを送れるようにするため、キー入力を受け取る。
    return YES;
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_tracking) {
        [self removeTrackingArea:_tracking];
        [_tracking release];
    }
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                     NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:_tracking];
}

- (void)releaseMipmaps {
    for (int i = 0; i < kMipLevels; ++i) {
        if (_mips[i]) CGImageRelease(_mips[i]);
        _mips[i] = NULL;
    }
}

- (BOOL)hasImage {
    return _image != NULL;
}

- (int)imageWidth {
    return _imageWidth;
}

- (int)imageHeight {
    return _imageHeight;
}

- (void)clearImage {
    if (_image) {
        CGImageRelease(_image);
        _image = NULL;
    }
    [self releaseMipmaps];
    _source.reset();
    _imageWidth = 0;
    _imageHeight = 0;
    _pan = NSZeroPoint;
    [self setNeedsDisplay:YES];
}

- (void)setDisplayStretch:(BOOL)on {
    _displayStretch = on;
    if (_source) [self rebuildImage];
}

- (void)setFixedStretchLow:(float)lo high:(float)hi gamma:(float)gamma {
    const BOOL changed = !_fixedStretch || lo != _fixedLo || hi != _fixedHi || gamma != _fixedGamma;
    _fixedStretch = YES;
    _fixedLo = lo;
    _fixedHi = hi > lo ? hi : lo + 1e-6f;
    _fixedGamma = gamma > 0.0f ? gamma : 1.0f;
    if (changed && _source) [self rebuildImage];
}

- (void)clearFixedStretch {
    if (!_fixedStretch) return;
    _fixedStretch = NO;
    if (_source) [self rebuildImage];
}

- (void)showFrameBuffer:(const stackcore::FrameBuffer&)frame {
    if (frame.empty()) {
        [self clearImage];
        return;
    }
    auto copy = std::make_shared<stackcore::FrameBuffer>(frame.width(), frame.height(),
                                                         frame.channels());
    for (int c = 0; c < frame.channels(); ++c) {
        for (int y = 0; y < frame.height(); ++y) {
            std::copy(frame.row(c, y), frame.row(c, y) + frame.width(), copy->row(c, y));
        }
    }
    [self showSharedFrame:copy];
}

- (void)showSharedFrame:(std::shared_ptr<const stackcore::FrameBuffer>)frame {
    if (!frame || frame->empty()) {
        [self clearImage];
        return;
    }
    _source = frame;
    [self rebuildImage];
}

- (void)rebuildImage {
    if (_image) {
        CGImageRelease(_image);
        _image = NULL;
    }
    [self releaseMipmaps];
    if (!_source || _source->empty()) {
        [self setNeedsDisplay:YES];
        return;
    }
    const stackcore::FrameBuffer& frame = *_source;
    const int w = frame.width();
    const int h = frame.height();
    const int channels = frame.channels();
    _imageWidth = w;
    _imageHeight = h;

    // 表示用のストレッチ係数。全チャンネル共通にしないと色が転ぶ。
    // 行ごとの最小・最大を並列に求めてから畳む（結果は実行順によらない）。
    float lo = 0.0f, hi = 1.0f;
    float displayGamma = 0.75f;
    if (_displayStretch && _fixedStretch) {
        lo = _fixedLo;
        hi = _fixedHi;
        displayGamma = _fixedGamma;
    } else if (_displayStretch) {
        std::vector<float> row_lo(static_cast<std::size_t>(h), 1.0f);
        std::vector<float> row_hi(static_cast<std::size_t>(h), 0.0f);
        float* rlo = row_lo.data();
        float* rhi = row_hi.data();
        const stackcore::FrameBuffer* f = &frame;
        dispatch_apply(static_cast<size_t>(h), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0),
                       ^(size_t y) {
                           float a = 1.0f, b = 0.0f;
                           for (int c = 0; c < channels; ++c) {
                               const float* r = f->row(c, static_cast<int>(y));
                               for (int x = 0; x < w; ++x) {
                                   if (r[x] < a) a = r[x];
                                   if (r[x] > b) b = r[x];
                               }
                           }
                           rlo[y] = a;
                           rhi[y] = b;
                       });
        lo = *std::min_element(row_lo.begin(), row_lo.end());
        hi = *std::max_element(row_hi.begin(), row_hi.end());
        if (!(hi > lo)) {
            lo = 0.0f;
            hi = 1.0f;
        }
    }
    const float inv_range = 1.0f / (hi - lo);
    const bool stretch = _displayStretch ? true : false;

    // 8bitのRGBAに落として CGImage を作る。
    // プレビューは目で見るためのものなので8bitで足りる。
    // 保存は16bit/32bit floatで別途行う。
    // ガンマ変換は4096段の表を引く（1画素ごとに pow を呼ぶと大画像で遅い）。
    // 軽いガンマで暗部を持ち上げる。惑星面の縞は中間調にある。
    constexpr int kLutSize = 4096;
    std::vector<unsigned char> lut(kLutSize + 1);
    for (int i = 0; i <= kLutSize; ++i) {
        const float t = static_cast<float>(i) / kLutSize;
        lut[static_cast<std::size_t>(i)] =
            static_cast<unsigned char>((stretch ? std::pow(t, displayGamma) : t) * 255.0f + 0.5f);
    }
    const unsigned char* table = lut.data();
    std::vector<unsigned char> pixels(static_cast<std::size_t>(w) * h * 4);
    unsigned char* base = pixels.data();
    const stackcore::FrameBuffer* f = &frame;
    dispatch_apply(static_cast<size_t>(h), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0),
                   ^(size_t yy) {
                       const int y = static_cast<int>(yy);
                       unsigned char* dst = base + static_cast<std::size_t>(y) * w * 4;
                       const auto map = [lo, inv_range, stretch, table](float v) -> unsigned char {
                           float t = stretch ? (v - lo) * inv_range : v;
                           if (!(t > 0.0f)) t = 0.0f;
                           if (t > 1.0f) t = 1.0f;
                           return table[static_cast<int>(t * kLutSize + 0.5f)];
                       };
                       if (channels >= 3) {
                           const float* r = f->row(0, y);
                           const float* g = f->row(1, y);
                           const float* b = f->row(2, y);
                           for (int x = 0; x < w; ++x) {
                               dst[x * 4 + 0] = map(r[x]);
                               dst[x * 4 + 1] = map(g[x]);
                               dst[x * 4 + 2] = map(b[x]);
                               dst[x * 4 + 3] = 255;
                           }
                       } else {
                           const float* v = f->row(0, y);
                           for (int x = 0; x < w; ++x) {
                               const unsigned char g = map(v[x]);
                               dst[x * 4 + 0] = g;
                               dst[x * 4 + 1] = g;
                               dst[x * 4 + 2] = g;
                               dst[x * 4 + 3] = 255;
                           }
                       }
                   });

    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(pixels.data(), w, h, 8, w * 4, space,
                                             kCGImageAlphaNoneSkipLast);
    if (ctx) {
        _image = CGBitmapContextCreateImage(ctx);
        CGContextRelease(ctx);
    }
    CGColorSpaceRelease(space);
    [self setNeedsDisplay:YES];
}

// 縮小表示用の画像（UI設計書 §5.3 のミップマップ相当）。
// 大きな画像を毎回全画素から縮めて描くと重いので、段ごとに1回だけ作って使い回す。
- (CGImageRef)imageForDeviceScale:(double)deviceScale {
    if (!_image || deviceScale >= 0.5) return _image;
    int level = 0;
    double s = deviceScale;
    while (s < 0.5 && level < kMipLevels) {
        s *= 2.0;
        ++level;
    }
    if (level == 0) return _image;
    CGImageRef& slot = _mips[level - 1];
    if (!slot) {
        const int w = std::max(1, _imageWidth >> level);
        const int h = std::max(1, _imageHeight >> level);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(NULL, w, h, 8, 0, space, kCGImageAlphaNoneSkipLast);
        if (ctx) {
            CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
            CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), _image);
            slot = CGBitmapContextCreateImage(ctx);
            CGContextRelease(ctx);
        }
        CGColorSpaceRelease(space);
    }
    return slot ? slot : _image;
}

- (void)setZoom:(double)zoom {
    _zoom = zoom <= 0.0 ? 0.0 : std::min(kMaxZoom, std::max(kMinZoom, zoom));
    if (_zoom <= 0.0) {
        _pan = NSZeroPoint;  // 「合わせる」に戻したらパンも戻す
    } else {
        // 倍率を下げたとき、前の倍率でのパン量が残っていると
        // 画像が視界の外に置き去りになる。新しい倍率の範囲に収め直す。
        _pan = [self clampedPan:_pan];
    }
    [self setNeedsDisplay:YES];
}

- (void)setZoom:(double)zoom keepingImagePoint:(NSPoint)p atViewPoint:(NSPoint)anchor {
    _zoom = std::min(kMaxZoom, std::max(kMinZoom, zoom));
    const NSRect bounds = [self bounds];
    const double scale = [self effectiveScale];
    const double drawW = _imageWidth * scale;
    const double drawH = _imageHeight * scale;
    const double originX = anchor.x - p.x * scale;
    const double originY = anchor.y + p.y * scale - drawH;
    _pan = [self clampedPan:NSMakePoint(originX - (NSMidX(bounds) - drawW * 0.5),
                                        originY - (NSMidY(bounds) - drawH * 0.5))];
    [self setNeedsDisplay:YES];
    if ([_delegate respondsToSelector:@selector(previewViewZoomDidChange:)]) {
        [_delegate previewViewZoomDidChange:self];
    }
}

- (double)effectiveDeviceZoom {
    return [self effectiveScale] * [self backingScale];
}

// 1段 = √2倍。2回で2倍になる。
- (void)zoomInStep {
    if (!_image) return;
    const NSRect b = [self bounds];
    const NSPoint center = NSMakePoint(NSMidX(b), NSMidY(b));
    [self setZoom:[self effectiveDeviceZoom] * M_SQRT2
        keepingImagePoint:[self imagePointFromViewPoint:center]
              atViewPoint:center];
}

- (void)zoomOutStep {
    if (!_image) return;
    const NSRect b = [self bounds];
    const NSPoint center = NSMakePoint(NSMidX(b), NSMidY(b));
    [self setZoom:[self effectiveDeviceZoom] / M_SQRT2
        keepingImagePoint:[self imagePointFromViewPoint:center]
              atViewPoint:center];
}

// パン量を「画像の端がビューの端より内側に入らない」範囲に収める。
// 画像がビューより小さい軸はパンできない（中央固定）。
// これが無いと、ドラッグで画像を完全に画面外へ捨てられてしまう。
- (NSPoint)clampedPan:(NSPoint)pan {
    if (_imageWidth == 0 || _imageHeight == 0) return NSZeroPoint;
    const NSRect bounds = [self bounds];
    const double scale = [self effectiveScale];
    const double maxX = std::max(0.0, (_imageWidth * scale - bounds.size.width) * 0.5);
    const double maxY = std::max(0.0, (_imageHeight * scale - bounds.size.height) * 0.5);
    return NSMakePoint(std::min(maxX, std::max(-maxX, static_cast<double>(pan.x))),
                       std::min(maxY, std::max(-maxY, static_cast<double>(pan.y))));
}

// ---- APオーバーレイ --------------------------------------------------------

- (void)setAlignmentPoints:(const std::vector<stackcore::AlignmentPoint>&)points
                    apSize:(int)apSize
             meanQualities:(const std::vector<double>&)meanQualities
           coordinateScale:(double)coordinateScale {
    _points = points;
    _apSize = apSize;
    _apScale = coordinateScale > 0.0 ? coordinateScale : 1.0;
    _selectedAp = -1;

    // ヒートマップ用に 0..1 へ正規化しておく。
    // 品質の絶対値は露出やビット深度で変わるので、色に直接使えない。
    _apQuality.clear();
    if (meanQualities.size() == points.size() && !points.empty()) {
        double lo = meanQualities[0], hi = meanQualities[0];
        for (std::size_t i = 1; i < meanQualities.size(); ++i) {
            lo = std::min(lo, meanQualities[i]);
            hi = std::max(hi, meanQualities[i]);
        }
        _apQuality.resize(meanQualities.size());
        for (std::size_t i = 0; i < meanQualities.size(); ++i) {
            _apQuality[i] = (hi > lo) ? (meanQualities[i] - lo) / (hi - lo) : 0.5;
        }
    }
    [self setNeedsDisplay:YES];
}

- (void)clearAlignmentPoints {
    _points.clear();
    _apQuality.clear();
    _selectedAp = -1;
    [self setNeedsDisplay:YES];
}

- (void)setShowAlignmentPoints:(BOOL)on {
    _showAlignmentPoints = on;
    [self setNeedsDisplay:YES];
}

- (void)setApHeatmap:(BOOL)on {
    _apHeatmap = on;
    [self setNeedsDisplay:YES];
}

- (void)setApEditing:(BOOL)on {
    _apEditing = on;
    if (!on) _selectedAp = -1;
    [self setNeedsDisplay:YES];
}

- (void)setCropOverlay:(NSRect)rect {
    _cropOverlay = rect;
    [self setNeedsDisplay:YES];
}

// ---- 座標変換 --------------------------------------------------------------

- (double)backingScale {
    NSWindow* window = [self window];
    const double s = window ? [window backingScaleFactor] : 1.0;
    return s > 0.0 ? s : 1.0;
}

// ビュー座標（ポイント）で、画像1画素が何ポイントになるか。
- (double)effectiveScale {
    if (_imageWidth == 0 || _imageHeight == 0) return 1.0;
    if (_zoom > 0.0) return _zoom / [self backingScale];
    // ウィンドウに合わせる。小さな惑星画像は拡大して画面を使い切る。
    const NSRect bounds = [self bounds];
    const double sx = bounds.size.width / _imageWidth;
    const double sy = bounds.size.height / _imageHeight;
    return std::min(sx, sy);
}

- (NSRect)imageDrawRect {
    const NSRect bounds = [self bounds];
    if (_imageWidth == 0) return NSZeroRect;
    const double scale = [self effectiveScale];
    const double drawW = _imageWidth * scale;
    const double drawH = _imageHeight * scale;
    // ウィンドウのリサイズでもパンの許容範囲は変わるので、
    // 保存済みの値をそのまま信じず、使う瞬間に必ず収め直す。
    const NSPoint pan = [self clampedPan:_pan];
    return NSMakeRect(NSMidX(bounds) - drawW * 0.5 + pan.x,
                      NSMidY(bounds) - drawH * 0.5 + pan.y, drawW, drawH);
}

- (NSPoint)imagePointFromViewPoint:(NSPoint)p {
    const NSRect r = [self imageDrawRect];
    const double scale = [self effectiveScale];
    if (r.size.width <= 0.0 || scale <= 0.0) return NSZeroPoint;
    // CGImage の 0 行目は**上端**。NSView の原点は左下。ここで上下を返す。
    return NSMakePoint((p.x - r.origin.x) / scale, (NSMaxY(r) - p.y) / scale);
}

- (NSPoint)viewPointFromImagePoint:(NSPoint)p {
    const NSRect r = [self imageDrawRect];
    const double scale = [self effectiveScale];
    return NSMakePoint(r.origin.x + p.x * scale, NSMaxY(r) - p.y * scale);
}

// ---- 描画 ------------------------------------------------------------------

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    const NSRect bounds = [self bounds];

    // **自分の領域の外へ描かない。**
    // 2倍/4倍では画像がビューより大きくなるが、AppKitはbounds外への描画を
    // 常にクリップしてくれるわけではない（レイヤー裏付けやスナップショット
    // 経路 cacheDisplayInRect では素通しになり、隣のペインやボタンの上に
    // 画像が覆い被さる）。ここで明示的にクリップする。
    [NSBezierPath clipRect:bounds];

    [[NSColor colorWithCalibratedWhite:0.12 alpha:1.0] setFill];
    NSRectFill(bounds);

    if (!_image || _imageWidth == 0) {
        NSString* message = LSLocalizedString(@"動画を追加して［品質評価］を押してください");
        NSMutableParagraphStyle* style =
            [[[NSMutableParagraphStyle alloc] init] autorelease];
        [style setAlignment:NSTextAlignmentCenter];
        NSDictionary* attrs = @{
            NSForegroundColorAttributeName : [NSColor colorWithCalibratedWhite:0.6 alpha:1.0],
            NSFontAttributeName : [NSFont systemFontOfSize:13.0],
            NSParagraphStyleAttributeName : style
        };
        const NSRect textRect =
            NSMakeRect(bounds.origin.x, NSMidY(bounds) - 10.0, bounds.size.width, 20.0);
        [message drawInRect:textRect withAttributes:attrs];
        return;
    }

    const NSRect target = [self imageDrawRect];
    const double deviceScale = [self effectiveDeviceZoom];

    CGContextRef ctx = (CGContextRef)[[NSGraphicsContext currentContext] graphicsPort];
    CGContextSaveGState(ctx);
    // 画面の実画素で等倍以上に拡大するときは補間しない。
    // 滑らかに補間すると「実際に写っている画素」が分からなくなる。
    CGContextSetInterpolationQuality(ctx, deviceScale >= 1.0 ? kCGInterpolationNone
                                                             : kCGInterpolationHigh);
    CGContextDrawImage(ctx, NSRectToCGRect(target), [self imageForDeviceScale:deviceScale]);
    CGContextRestoreGState(ctx);

    if (_showAlignmentPoints && !_points.empty()) [self drawAlignmentPoints];
    if (_cropOverlay.size.width > 0.0 && _cropOverlay.size.height > 0.0) [self drawCropOverlay];
    if (_showAlignmentPoints && _apHeatmap && !_apQuality.empty()) [self drawHeatmapLegend];
}

- (void)drawCropOverlay {
    const NSPoint a = [self viewPointFromImagePoint:_cropOverlay.origin];
    const NSPoint b = [self viewPointFromImagePoint:NSMakePoint(NSMaxX(_cropOverlay),
                                                                NSMaxY(_cropOverlay))];
    const NSRect r = NSMakeRect(std::min(a.x, b.x), std::min(a.y, b.y), std::fabs(b.x - a.x),
                                std::fabs(b.y - a.y));
    NSBezierPath* path = [NSBezierPath bezierPathWithRect:r];
    const CGFloat dash[2] = {6.0, 4.0};
    [path setLineDash:dash count:2 phase:0.0];
    [path setLineWidth:1.5];
    [[NSColor colorWithCalibratedRed:1.0 green:0.85 blue:0.2 alpha:0.95] setStroke];
    [path stroke];
}

// 品質の色分けの凡例。色だけでは何が高いのか分からない。
- (void)drawHeatmapLegend {
    const NSRect bounds = [self bounds];
    const NSRect bar = NSMakeRect(bounds.origin.x + 12.0, bounds.origin.y + 12.0, 120.0, 8.0);
    [[NSColor colorWithCalibratedWhite:0.0 alpha:0.55] setFill];
    NSRectFillUsingOperation(NSInsetRect(bar, -8.0, -14.0), NSCompositingOperationSourceOver);
    for (int i = 0; i < 120; ++i) {
        const double t = i / 119.0;
        [[NSColor colorWithCalibratedHue:(1.0 - t) * 0.6 saturation:0.9 brightness:1.0 alpha:1.0]
            setFill];
        NSRectFill(NSMakeRect(bar.origin.x + i, bar.origin.y, 1.0, bar.size.height));
    }
    NSDictionary* attrs = @{
        NSForegroundColorAttributeName : [NSColor whiteColor],
        NSFontAttributeName : [NSFont systemFontOfSize:9.0]
    };
    [LSLocalizedString(@"品質 低") drawAtPoint:NSMakePoint(bar.origin.x, NSMaxY(bar) + 1.0)
                               withAttributes:attrs];
    NSString* high = LSLocalizedString(@"高");
    const NSSize size = [high sizeWithAttributes:attrs];
    [high drawAtPoint:NSMakePoint(NSMaxX(bar) - size.width, NSMaxY(bar) + 1.0) withAttributes:attrs];
}

- (void)drawAlignmentPoints {
    const double scale = [self effectiveScale];
    const double side = _apSize * _apScale * scale;
    if (side < 2.0) return;  // 縮小しすぎて枠が潰れるなら描かない

    const BOOL heat = _apHeatmap && !_apQuality.empty();

    for (std::size_t i = 0; i < _points.size(); ++i) {
        const double cx = _points[i].cx * _apScale;
        const double cy = _points[i].cy * _apScale;
        const NSPoint center = [self viewPointFromImagePoint:NSMakePoint(cx, cy)];
        const NSRect box = NSMakeRect(center.x - side * 0.5, center.y - side * 0.5, side, side);

        NSColor* color;
        if (heat) {
            // 寒色→暖色。品質が高いほど暖かい。
            const double t = _apQuality[i];
            color = [NSColor colorWithCalibratedHue:(1.0 - t) * 0.6
                                         saturation:0.9
                                         brightness:1.0
                                              alpha:0.9];
        } else {
            color = [NSColor colorWithCalibratedRed:0.35 green:0.85 blue:1.0 alpha:0.75];
        }

        const BOOL selected = (static_cast<NSInteger>(i) == _selectedAp);
        if (selected) {
            [[NSColor colorWithCalibratedRed:1.0 green:0.85 blue:0.2 alpha:0.25] setFill];
            NSRectFillUsingOperation(box, NSCompositingOperationSourceOver);
            color = [NSColor colorWithCalibratedRed:1.0 green:0.85 blue:0.2 alpha:1.0];
        }

        [color setStroke];
        NSBezierPath* path = [NSBezierPath bezierPathWithRect:box];
        [path setLineWidth:selected ? 2.0 : 1.0];
        [path stroke];

        // 中心の十字。枠だけだと、重なったAPの中心がどこか分からない。
        NSBezierPath* cross = [NSBezierPath bezierPath];
        [cross moveToPoint:NSMakePoint(center.x - 3.0, center.y)];
        [cross lineToPoint:NSMakePoint(center.x + 3.0, center.y)];
        [cross moveToPoint:NSMakePoint(center.x, center.y - 3.0)];
        [cross lineToPoint:NSMakePoint(center.x, center.y + 3.0)];
        [cross setLineWidth:1.0];
        [cross stroke];
    }
}

// ---- 操作 ------------------------------------------------------------------

- (NSInteger)alignmentPointCount {
    return static_cast<NSInteger>(_points.size());
}

// クリック位置にいちばん近いAP。範囲外なら -1。
- (NSInteger)apIndexAtViewPoint:(NSPoint)p {
    if (_points.empty()) return -1;
    const NSPoint img = [self imagePointFromViewPoint:p];
    const double half = _apSize * 0.5;
    NSInteger best = -1;
    double bestDist = 0.0;
    for (std::size_t i = 0; i < _points.size(); ++i) {
        const double dx = img.x / _apScale - _points[i].cx;
        const double dy = img.y / _apScale - _points[i].cy;
        if (std::fabs(dx) > half || std::fabs(dy) > half) continue;
        const double d = dx * dx + dy * dy;
        if (best < 0 || d < bestDist) {
            best = static_cast<NSInteger>(i);
            bestDist = d;
        }
    }
    return best;
}

- (void)mouseDown:(NSEvent*)event {
    const NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    [[self window] makeFirstResponder:self];

    if (_apEditing && _image) {
        const NSInteger hit = [self apIndexAtViewPoint:p];
        if (hit >= 0) {
            _selectedAp = hit;
            [self setNeedsDisplay:YES];
            return;
        }
        const NSPoint img = [self imagePointFromViewPoint:p];
        if (img.x < 0 || img.y < 0 || img.x >= _imageWidth || img.y >= _imageHeight) return;
        if (_delegate) {
            [_delegate previewView:self
                       didAddApAtX:static_cast<int>(std::lround(img.x / _apScale))
                                 y:static_cast<int>(std::lround(img.y / _apScale))];
        }
        return;
    }

    // AP編集でなければドラッグはパン。
    _panning = YES;
    _panStart = p;
    _panOrigin = _pan;
}

- (void)mouseDragged:(NSEvent*)event {
    if (!_panning) return;
    const NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    // クランプした値を保存する。生の値を貯めると、限界を超えてドラッグした分だけ
    // 戻すときに「効かない区間」ができて、操作が引っかかったように感じる。
    _pan = [self clampedPan:NSMakePoint(_panOrigin.x + (p.x - _panStart.x),
                                        _panOrigin.y + (p.y - _panStart.y))];
    [self setNeedsDisplay:YES];
}

- (void)mouseUp:(NSEvent*)event {
    (void)event;
    _panning = NO;
}

- (void)rightMouseDown:(NSEvent*)event {
    if (!_apEditing) return;
    const NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    const NSInteger hit = [self apIndexAtViewPoint:p];
    if (hit >= 0 && _delegate) [_delegate previewView:self didDeleteApAtIndex:hit];
}

// ホイール: 拡大中はパン、⌘を押しながらならカーソル位置を中心にズーム。
- (void)scrollWheel:(NSEvent*)event {
    if (!_image) return;
    const NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    if ([event modifierFlags] & NSEventModifierFlagCommand) {
        const double delta = [event hasPreciseScrollingDeltas] ? [event scrollingDeltaY] / 50.0
                                                                : [event scrollingDeltaY] / 5.0;
        const double factor = std::pow(2.0, delta);
        [self setZoom:[self effectiveDeviceZoom] * factor
            keepingImagePoint:[self imagePointFromViewPoint:p]
                  atViewPoint:p];
        return;
    }
    const double k = [event hasPreciseScrollingDeltas] ? 1.0 : 8.0;
    _pan = [self clampedPan:NSMakePoint(_pan.x + [event scrollingDeltaX] * k,
                                        _pan.y - [event scrollingDeltaY] * k)];
    [self setNeedsDisplay:YES];
}

// トラックパッドのピンチ。
- (void)magnifyWithEvent:(NSEvent*)event {
    if (!_image) return;
    const NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    [self setZoom:[self effectiveDeviceZoom] * (1.0 + [event magnification])
        keepingImagePoint:[self imagePointFromViewPoint:p]
              atViewPoint:p];
}

- (void)mouseMoved:(NSEvent*)event {
    if (![_delegate respondsToSelector:@selector(previewView:hoverDescription:)]) return;
    const NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    NSString* text = nil;
    if (_source && _image) {
        const NSPoint img = [self imagePointFromViewPoint:p];
        const int x = static_cast<int>(std::floor(img.x));
        const int y = static_cast<int>(std::floor(img.y));
        if (x >= 0 && y >= 0 && x < _source->width() && y < _source->height()) {
            if (_source->channels() >= 3) {
                text = [NSString stringWithFormat:@"x %d  y %d   R %.4f  G %.4f  B %.4f", x, y,
                                                  _source->row(0, y)[x], _source->row(1, y)[x],
                                                  _source->row(2, y)[x]];
            } else {
                text = [NSString stringWithFormat:@"x %d  y %d   %.4f", x, y,
                                                  _source->row(0, y)[x]];
            }
        }
    }
    [_delegate previewView:self hoverDescription:text];
}

- (void)mouseExited:(NSEvent*)event {
    (void)event;
    if ([_delegate respondsToSelector:@selector(previewView:hoverDescription:)]) {
        [_delegate previewView:self hoverDescription:nil];
    }
}

- (void)keyDown:(NSEvent*)event {
    const unichar key = [[event charactersIgnoringModifiers] length] > 0
                            ? [[event charactersIgnoringModifiers] characterAtIndex:0]
                            : 0;
    if ((key == NSDeleteCharacter || key == NSDeleteFunctionKey) && _selectedAp >= 0) {
        const NSInteger index = _selectedAp;
        _selectedAp = -1;
        if (_delegate) [_delegate previewView:self didDeleteApAtIndex:index];
        return;
    }
    if ((key == NSLeftArrowFunctionKey || key == NSRightArrowFunctionKey) &&
        [_delegate respondsToSelector:@selector(previewView:didRequestFrameStep:)]) {
        int step = key == NSLeftArrowFunctionKey ? -1 : 1;
        if ([event modifierFlags] & NSEventModifierFlagShift) step *= 10;
        if ([event modifierFlags] & NSEventModifierFlagOption) step *= 100;
        [_delegate previewView:self didRequestFrameStep:step];
        return;
    }
    [super keyDown:event];
}

@end
