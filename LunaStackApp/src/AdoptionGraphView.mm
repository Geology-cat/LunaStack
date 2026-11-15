#import "AdoptionGraphView.h"

#include <algorithm>
#include <vector>

@implementation AdoptionGraphView {
    std::vector<double> _percents;
    std::vector<double> _scores;
    double _suggested;
    double _currentPercent;
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        _suggested = -1.0;
        _currentPercent = -1.0;
    }
    return self;
}

- (BOOL)isFlipped {
    return NO;
}

- (void)setPercents:(const double*)percents
             scores:(const double*)scores
              count:(int)count
          suggested:(double)suggested {
    _percents.assign(percents, percents + count);
    _scores.assign(scores, scores + count);
    _suggested = suggested;
    [self setNeedsDisplay:YES];
}

- (void)clearData {
    _percents.clear();
    _scores.clear();
    _suggested = -1.0;
    [self setNeedsDisplay:YES];
}

- (BOOL)hasData {
    return !_percents.empty();
}

- (double)currentPercent {
    return _currentPercent;
}

- (void)setCurrentPercent:(double)percent {
    if (_currentPercent == percent) return;
    _currentPercent = percent;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    const NSRect bounds = [self bounds];
    [[NSColor textBackgroundColor] setFill];
    NSRectFill(bounds);
    [[NSColor gridColor] setStroke];
    [NSBezierPath strokeRect:NSInsetRect(bounds, 0.5, 0.5)];
    if (_percents.empty()) return;

    // 描く範囲。下に採用率の目盛り、上に目安の文字を置く余白を取る。
    const NSRect plot = NSMakeRect(NSMinX(bounds) + 10.0, NSMinY(bounds) + 20.0, NSWidth(bounds) - 20.0,
                                   NSHeight(bounds) - 38.0);
    // 縦軸は、最大の8割から最大までは必ず入れる（わずかな差を大きな差に見せない）。
    const double hi = *std::max_element(_scores.begin(), _scores.end());
    const double lo = std::min(*std::min_element(_scores.begin(), _scores.end()), 0.8 * hi);
    const double span = std::max(hi - lo, 1e-9);
    const auto px = [&](double percent) { return NSMinX(plot) + NSWidth(plot) * percent / 100.0; };
    const auto py = [&](double score) { return NSMinY(plot) + NSHeight(plot) * (score - lo) / span; };

    NSDictionary* small = @{
        NSFontAttributeName : [NSFont systemFontOfSize:9.0],
        NSForegroundColorAttributeName : [NSColor secondaryLabelColor]
    };
    // 横軸の目盛り。
    for (double tick : {0.0, 25.0, 50.0, 75.0, 100.0}) {
        const CGFloat x = px(tick);
        [[NSColor gridColor] setStroke];
        NSBezierPath* line = [NSBezierPath bezierPath];
        [line moveToPoint:NSMakePoint(x, NSMinY(plot) - 3.0)];
        [line lineToPoint:NSMakePoint(x, NSMaxY(plot))];
        [line setLineWidth:0.5];
        [line stroke];
        NSString* text = [NSString stringWithFormat:@"%.0f%%", tick];
        const NSSize size = [text sizeWithAttributes:small];
        CGFloat tx = x - size.width / 2.0;
        tx = std::max(NSMinX(bounds) + 2.0, std::min(NSMaxX(bounds) - size.width - 2.0, tx));
        [text drawAtPoint:NSMakePoint(tx, NSMinY(bounds) + 3.0) withAttributes:small];
    }

    // いまの採用率。
    if (_currentPercent >= 0.0) {
        const CGFloat x = px(std::min(100.0, _currentPercent));
        [[NSColor systemOrangeColor] setStroke];
        NSBezierPath* line = [NSBezierPath bezierPath];
        [line moveToPoint:NSMakePoint(x, NSMinY(plot) - 3.0)];
        [line lineToPoint:NSMakePoint(x, NSMaxY(plot) + 3.0)];
        [line setLineWidth:1.0];
        [line stroke];
    }

    // 折れ線と点。
    [[NSColor systemBlueColor] setStroke];
    NSBezierPath* curve = [NSBezierPath bezierPath];
    for (std::size_t i = 0; i < _percents.size(); ++i) {
        const NSPoint p = NSMakePoint(px(_percents[i]), py(_scores[i]));
        if (i == 0) [curve moveToPoint:p];
        else [curve lineToPoint:p];
    }
    [curve setLineWidth:1.5];
    [curve stroke];
    for (std::size_t i = 0; i < _percents.size(); ++i) {
        const BOOL suggested = _percents[i] == _suggested;
        const CGFloat r = suggested ? 4.5 : 2.5;
        const NSRect dot = NSMakeRect(px(_percents[i]) - r, py(_scores[i]) - r, 2.0 * r, 2.0 * r);
        [(suggested ? [NSColor systemGreenColor] : [NSColor systemBlueColor]) setFill];
        [[NSBezierPath bezierPathWithOvalInRect:dot] fill];
        if (suggested) {
            NSDictionary* bold = @{
                NSFontAttributeName : [NSFont boldSystemFontOfSize:10.0],
                NSForegroundColorAttributeName : [NSColor labelColor]
            };
            NSString* text = [NSString stringWithFormat:@"%.0f%%", _suggested];
            const NSSize size = [text sizeWithAttributes:bold];
            CGFloat tx = NSMidX(dot) - size.width / 2.0;
            tx = std::max(NSMinX(bounds) + 2.0, std::min(NSMaxX(bounds) - size.width - 2.0, tx));
            CGFloat ty = NSMaxY(dot) + 2.0;
            if (ty + size.height > NSMaxY(bounds) - 1.0) ty = NSMinY(dot) - size.height - 2.0;
            [text drawAtPoint:NSMakePoint(tx, ty) withAttributes:bold];
        }
    }
}

@end
