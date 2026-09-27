#import "LevelsView.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr CGFloat kInset = 7.0;         // 左右の余白（三角がはみ出さないように）
constexpr CGFloat kHandleHeight = 11.0;  // 三角の高さ
constexpr CGFloat kHandleStrip = 16.0;   // 三角を置く帯の高さ
constexpr double kMinGap = 2.0 / 255.0;  // 黒と白の最小の間隔

}  // namespace

double LSLevelsGammaFromMidpoint(double t) {
    t = std::max(0.001, std::min(0.999, t));
    return std::max(kLevelsGammaMin, std::min(kLevelsGammaMax, std::log(t) / std::log(0.5)));
}

double LSLevelsMidpointFromGamma(double gamma) {
    return std::pow(0.5, std::max(kLevelsGammaMin, std::min(kLevelsGammaMax, gamma)));
}

@implementation LevelsView {
    std::vector<std::uint32_t> _counts;
    int _dragging;  // 0=なし 1=黒 2=中間 3=白
}

@synthesize black = _black;
@synthesize white = _white;
@synthesize gamma = _gamma;
@synthesize enabled = _enabled;
@synthesize delegate = _delegate;

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        _black = 0.0;
        _white = 1.0;
        _gamma = 1.0;
        _enabled = YES;
        _dragging = 0;
    }
    return self;
}

- (BOOL)isOpaque {
    return NO;
}

- (NSSize)intrinsicContentSize {
    return NSMakeSize(NSViewNoIntrinsicMetric, 118.0);
}

- (void)setHistogram:(const std::vector<std::uint32_t>&)counts {
    _counts = counts;
    [self setNeedsDisplay:YES];
}

- (void)setBlack:(double)v {
    _black = v;
    [self setNeedsDisplay:YES];
}

- (void)setWhite:(double)v {
    _white = v;
    [self setNeedsDisplay:YES];
}

- (void)setGamma:(double)v {
    _gamma = v;
    [self setNeedsDisplay:YES];
}

- (void)setEnabled:(BOOL)on {
    _enabled = on;
    [self setNeedsDisplay:YES];
}

// ---- 座標 ----------------------------------------------------------------

- (NSRect)histogramRect {
    const NSRect b = [self bounds];
    return NSMakeRect(kInset, kHandleStrip + 2.0, b.size.width - 2.0 * kInset,
                      b.size.height - kHandleStrip - 3.0);
}

- (CGFloat)xForValue:(double)v {
    const NSRect r = [self histogramRect];
    return NSMinX(r) + static_cast<CGFloat>(std::max(0.0, std::min(1.0, v))) * r.size.width;
}

- (double)valueForX:(CGFloat)x {
    const NSRect r = [self histogramRect];
    return std::max(0.0, std::min(1.0, static_cast<double>((x - NSMinX(r)) / r.size.width)));
}

- (double)midValue {
    return _black + LSLevelsMidpointFromGamma(_gamma) * (_white - _black);
}

// ---- 描画 ----------------------------------------------------------------

- (void)drawTriangleAt:(CGFloat)x fill:(NSColor*)fill stroke:(NSColor*)stroke {
    NSBezierPath* p = [NSBezierPath bezierPath];
    [p moveToPoint:NSMakePoint(x, kHandleStrip - 1.0)];
    [p lineToPoint:NSMakePoint(x - 6.0, kHandleStrip - 1.0 - kHandleHeight)];
    [p lineToPoint:NSMakePoint(x + 6.0, kHandleStrip - 1.0 - kHandleHeight)];
    [p closePath];
    [fill setFill];
    [p fill];
    [stroke setStroke];
    [p setLineWidth:1.0];
    [p stroke];
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    const NSRect r = [self histogramRect];
    [[NSColor colorWithCalibratedWhite:0.27 alpha:1.0] setFill];
    NSRectFill(r);

    if (!_counts.empty()) {
        // 高い山（黒い空の0付近など）は Photoshop と同じく上で切る。
        // 3番目に高い段を上端にすると、1〜2本の突出した山に全体が潰されない。
        std::vector<std::uint32_t> sorted(_counts);
        std::sort(sorted.begin(), sorted.end(), std::greater<std::uint32_t>());
        const double top = std::max(1.0, static_cast<double>(sorted[std::min<std::size_t>(2, sorted.size() - 1)]));
        const std::size_t n = _counts.size();
        const CGFloat w = r.size.width / static_cast<CGFloat>(n);
        [[NSColor colorWithCalibratedWhite:_enabled ? 0.86 : 0.55 alpha:1.0] setFill];
        for (std::size_t i = 0; i < n; ++i) {
            const double h = std::min(1.0, _counts[i] / top);
            if (h <= 0.0) continue;
            NSRectFill(NSMakeRect(NSMinX(r) + w * static_cast<CGFloat>(i), NSMinY(r), std::max<CGFloat>(1.0, w),
                                  static_cast<CGFloat>(h) * r.size.height));
        }
    }

    const CGFloat alpha = _enabled ? 1.0 : 0.4;
    [self drawTriangleAt:[self xForValue:_black]
                    fill:[NSColor colorWithCalibratedWhite:0.05 alpha:alpha]
                  stroke:[NSColor colorWithCalibratedWhite:0.85 alpha:alpha]];
    [self drawTriangleAt:[self xForValue:[self midValue]]
                    fill:[NSColor colorWithCalibratedWhite:0.55 alpha:alpha]
                  stroke:[NSColor colorWithCalibratedWhite:0.25 alpha:alpha]];
    [self drawTriangleAt:[self xForValue:_white]
                    fill:[NSColor colorWithCalibratedWhite:0.97 alpha:alpha]
                  stroke:[NSColor colorWithCalibratedWhite:0.35 alpha:alpha]];
}

// ---- 操作 ----------------------------------------------------------------

- (void)mouseDown:(NSEvent*)event {
    if (!_enabled) return;
    const NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    const CGFloat xs[3] = {[self xForValue:_black], [self xForValue:[self midValue]], [self xForValue:_white]};
    // いちばん近い三角をつかむ（重なっているときは、動かせる向きのある方を選ぶ）。
    int best = 0;
    CGFloat bestDistance = 1e9;
    for (int i = 0; i < 3; ++i) {
        const CGFloat d = std::fabs(p.x - xs[i]);
        if (d < bestDistance - 0.01 || (std::fabs(d - bestDistance) <= 0.01 && i == 1)) {
            bestDistance = d;
            best = i;
        }
    }
    if (bestDistance > 10.0) return;
    if (best == 0 && xs[0] == xs[2] && p.x > xs[0]) best = 2;
    _dragging = best + 1;
    [self mouseDragged:event];
}

- (void)mouseDragged:(NSEvent*)event {
    if (!_dragging) return;
    const NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    const double v = [self valueForX:p.x];
    if (_dragging == 1) {
        _black = std::max(0.0, std::min(v, _white - kMinGap));
    } else if (_dragging == 3) {
        _white = std::min(1.0, std::max(v, _black + kMinGap));
    } else {
        const double t = (v - _black) / std::max(1e-9, _white - _black);
        _gamma = std::round(LSLevelsGammaFromMidpoint(t) * 100.0) / 100.0;
    }
    [self setNeedsDisplay:YES];
    [_delegate levelsViewDidChange:self];
}

- (void)mouseUp:(NSEvent*)event {
    (void)event;
    _dragging = 0;
}

@end
