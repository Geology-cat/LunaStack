#import "PreviewView.h"

#import "Localization.h"

#include <algorithm>
#include <cmath>
#include <vector>

// 定義より前に出てくるメソッドの宣言。無いと型が分からず素通りしてしまう。
@interface PreviewView ()
- (NSPoint)clampedPan:(NSPoint)pan;
@end

@implementation PreviewView {
    CGImageRef _image;
    int _imageWidth;
    int _imageHeight;
    // 直近に渡された画像。ストレッチの切り替えで作り直すために持っておく。
    stackcore::FrameBuffer _source;

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
}

@synthesize zoom = _zoom;
@synthesize displayStretch = _displayStretch;
@synthesize showAlignmentPoints = _showAlignmentPoints;
@synthesize apHeatmap = _apHeatmap;
@synthesize apEditing = _apEditing;
@synthesize delegate = _delegate;

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        _image = NULL;
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
    }
    return self;
}

- (void)dealloc {
    if (_image) CGImageRelease(_image);
    [super dealloc];
}

- (BOOL)isOpaque {
    return YES;
}

- (BOOL)acceptsFirstResponder {
    // Delete キーでAPを消せるようにするため、キー入力を受け取る。
    return YES;
}

- (void)clearImage {
    if (_image) {
        CGImageRelease(_image);
        _image = NULL;
    }
    _source.clear();
    _imageWidth = 0;
    _imageHeight = 0;
    _pan = NSZeroPoint;
    [self setNeedsDisplay:YES];
}

- (void)setDisplayStretch:(BOOL)on {
    _displayStretch = on;
    if (!_source.empty()) [self rebuildImage];
}

- (void)showFrameBuffer:(const stackcore::FrameBuffer&)frame {
    if (frame.empty()) {
        [self clearImage];
        return;
    }
    // ストレッチの切り替えで作り直せるよう、元の値を保持する。
    _source.reset(frame.width(), frame.height(), frame.channels());
    for (int c = 0; c < frame.channels(); ++c) {
        for (int y = 0; y < frame.height(); ++y) {
            const float* s = frame.row(c, y);
            float* d = _source.row(c, y);
            for (int x = 0; x < frame.width(); ++x) d[x] = s[x];
        }
    }
    [self rebuildImage];
}

- (void)rebuildImage {
    const stackcore::FrameBuffer& frame = _source;
    if (_image) {
        CGImageRelease(_image);
        _image = NULL;
    }
    if (frame.empty()) {
        [self setNeedsDisplay:YES];
        return;
    }

    const int w = frame.width();
    const int h = frame.height();
    const int channels = frame.channels();
    _imageWidth = w;
    _imageHeight = h;

    // 表示用のストレッチ係数。全チャンネル共通にしないと色が転ぶ。
    float lo = 0.0f, hi = 1.0f;
    if (_displayStretch) {
        lo = 1.0f;
        hi = 0.0f;
        for (int c = 0; c < channels; ++c) {
            for (int y = 0; y < h; ++y) {
                const float* r = frame.row(c, y);
                for (int x = 0; x < w; ++x) {
                    if (r[x] < lo) lo = r[x];
                    if (r[x] > hi) hi = r[x];
                }
            }
        }
        if (!(hi > lo)) {
            lo = 0.0f;
            hi = 1.0f;
        }
    }
    const float inv_range = 1.0f / (hi - lo);
    const bool stretch = _displayStretch ? true : false;
    auto map = [lo, inv_range, stretch](float v) -> float {
        if (!stretch) return v;
        float t = (v - lo) * inv_range;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        // 軽いガンマで暗部を持ち上げる。惑星面の縞は中間調にある。
        return std::pow(t, 0.75f);
    };

    // 8bitのRGBAに落として CGImage を作る。
    // プレビューは目で見るためのものなので8bitで足りる。
    // 保存は16bit/32bit floatのTIFFで別途行う。
    std::vector<unsigned char> pixels(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        unsigned char* dst = pixels.data() + static_cast<std::size_t>(y) * w * 4;
        if (channels >= 3) {
            const float* r = frame.row(0, y);
            const float* g = frame.row(1, y);
            const float* b = frame.row(2, y);
            for (int x = 0; x < w; ++x) {
                dst[x * 4 + 0] = static_cast<unsigned char>(
                    std::min(255.0f, std::max(0.0f, map(r[x]) * 255.0f + 0.5f)));
                dst[x * 4 + 1] = static_cast<unsigned char>(
                    std::min(255.0f, std::max(0.0f, map(g[x]) * 255.0f + 0.5f)));
                dst[x * 4 + 2] = static_cast<unsigned char>(
                    std::min(255.0f, std::max(0.0f, map(b[x]) * 255.0f + 0.5f)));
                dst[x * 4 + 3] = 255;
            }
        } else {
            const float* v = frame.row(0, y);
            for (int x = 0; x < w; ++x) {
                const unsigned char g = static_cast<unsigned char>(
                    std::min(255.0f, std::max(0.0f, map(v[x]) * 255.0f + 0.5f)));
                dst[x * 4 + 0] = g;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = g;
                dst[x * 4 + 3] = 255;
            }
        }
    }

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

- (void)setZoom:(double)zoom {
    _zoom = zoom;
    if (zoom <= 0.0) {
        _pan = NSZeroPoint;  // 「合わせる」に戻したらパンも戻す
    } else {
        // 倍率を下げたとき、前の倍率でのパン量が残っていると
        // 画像が視界の外に置き去りになる。新しい倍率の範囲に収め直す。
        _pan = [self clampedPan:_pan];
    }
    [self setNeedsDisplay:YES];
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

// ---- 座標変換 --------------------------------------------------------------

- (double)effectiveScale {
    if (_imageWidth == 0 || _imageHeight == 0) return 1.0;
    if (_zoom > 0.0) return _zoom;
    // ウィンドウに合わせる。拡大はしない（等倍を超えて引き伸ばさない）。
    const NSRect bounds = [self bounds];
    const double sx = bounds.size.width / _imageWidth;
    const double sy = bounds.size.height / _imageHeight;
    return std::min(1.0, std::min(sx, sy));
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
    const double scale = [self effectiveScale];

    CGContextRef ctx = (CGContextRef)[[NSGraphicsContext currentContext] graphicsPort];
    CGContextSaveGState(ctx);
    // 等倍以上に拡大するときは補間しない。
    // 滑らかに補間すると「実際に写っている画素」が分からなくなる。
    CGContextSetInterpolationQuality(ctx, scale >= 1.0 ? kCGInterpolationNone
                                                       : kCGInterpolationHigh);
    CGContextDrawImage(ctx, NSRectToCGRect(target), _image);
    CGContextRestoreGState(ctx);

    if (_showAlignmentPoints && !_points.empty()) [self drawAlignmentPoints];
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

    if (_apEditing && _image) {
        [[self window] makeFirstResponder:self];
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
    [super keyDown:event];
}

@end
