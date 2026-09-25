#import "QualityGraphView.h"

#import "Localization.h"

#include <algorithm>
#include <cmath>
#include <vector>

@implementation QualityGraphView {
    std::vector<double> _quality;    // フレーム番号順
    std::vector<unsigned char> _ok;  // 採用されたか
    // 採用フレームの品質を降順に並べたもの。カットラインの換算に使う。
    std::vector<double> _sortedAccepted;
    // 品質順表示の並び（採用フレームを品質降順、続けて除外フレームを番号順）。
    std::vector<int> _qualityOrder;
    double _minQ;
    double _maxQ;
    BOOL _dragging;
    int _hoverFrame;
    NSTrackingArea* _tracking;
}

@synthesize cutPercent = _cutPercent;
@synthesize cutLabel = _cutLabel;
@synthesize sortedByQuality = _sortedByQuality;
@synthesize currentFrame = _currentFrame;
@synthesize displayOffset = _displayOffset;
@synthesize delegate = _delegate;

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        _cutPercent = 25.0;
        _cutLabel = [@"" copy];
        _sortedByQuality = NO;
        _minQ = 0.0;
        _maxQ = 1.0;
        _dragging = NO;
        _currentFrame = -1;
        _hoverFrame = -1;
        _displayOffset = 0;
        _tracking = nil;
    }
    return self;
}

- (void)dealloc {
    [_cutLabel release];
    [_tracking release];
    [super dealloc];
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

- (BOOL)hasData {
    return !_quality.empty();
}

- (void)clearData {
    _quality.clear();
    _ok.clear();
    _sortedAccepted.clear();
    _qualityOrder.clear();
    _hoverFrame = -1;
    [self setNeedsDisplay:YES];
}

- (void)setQualities:(const double*)qualities
            accepted:(const unsigned char*)accepted
               count:(int)count {
    _quality.assign(qualities, qualities + count);
    _ok.assign(accepted, accepted + count);

    _sortedAccepted.clear();
    for (int i = 0; i < count; ++i) {
        if (_ok[static_cast<std::size_t>(i)]) _sortedAccepted.push_back(_quality[static_cast<std::size_t>(i)]);
    }
    std::sort(_sortedAccepted.begin(), _sortedAccepted.end(), std::greater<double>());

    // 品質順の並び。同点はフレーム番号の昇順（エンジンの select_top_frames と同じ）。
    _qualityOrder.clear();
    for (int i = 0; i < count; ++i) {
        if (_ok[static_cast<std::size_t>(i)]) _qualityOrder.push_back(i);
    }
    const std::vector<double>& q = _quality;
    std::sort(_qualityOrder.begin(), _qualityOrder.end(), [&q](int a, int b) {
        if (q[static_cast<std::size_t>(a)] != q[static_cast<std::size_t>(b)]) {
            return q[static_cast<std::size_t>(a)] > q[static_cast<std::size_t>(b)];
        }
        return a < b;
    });
    for (int i = 0; i < count; ++i) {
        if (!_ok[static_cast<std::size_t>(i)]) _qualityOrder.push_back(i);
    }

    // 縦軸の範囲は採用フレームから決める。
    // 除外フレームには極端な値が混じるので、これを入れると
    // 正常なフレームの差が潰れて何も見えなくなる。
    if (_sortedAccepted.empty()) {
        _minQ = 0.0;
        _maxQ = 1.0;
    } else {
        _maxQ = _sortedAccepted.front();
        _minQ = _sortedAccepted.back();
        if (!(_maxQ > _minQ)) {
            _maxQ = _minQ + 1e-9;
        }
    }
    [self setNeedsDisplay:YES];
}

- (void)setCutPercent:(double)percent {
    _cutPercent = std::min(100.0, std::max(1.0, percent));
    [self setNeedsDisplay:YES];
}

- (void)setCutLabel:(NSString*)label {
    if (label == _cutLabel) return;
    [_cutLabel release];
    _cutLabel = [label copy];
    [self setNeedsDisplay:YES];
}

- (void)setSortedByQuality:(BOOL)sorted {
    _sortedByQuality = sorted;
    [self setNeedsDisplay:YES];
}

- (void)setCurrentFrame:(int)frame {
    _currentFrame = frame;
    [self setNeedsDisplay:YES];
}

// 選択率に対応する品質の下限値。
// エンジンの select_top_frames と同じ数え方にする（採用フレームのうち上位N枚）。
- (double)cutQuality {
    if (_sortedAccepted.empty()) return _minQ;
    int keep = static_cast<int>(_sortedAccepted.size() * _cutPercent / 100.0 + 0.5);
    if (keep < 1) keep = 1;
    if (keep > static_cast<int>(_sortedAccepted.size())) {
        keep = static_cast<int>(_sortedAccepted.size());
    }
    return _sortedAccepted[static_cast<std::size_t>(keep - 1)];
}

- (NSRect)plotRect {
    const NSRect b = [self bounds];
    // 下端は横軸の数字、上端はカーソル位置の説明のために空ける。
    return NSMakeRect(b.origin.x + 2.0, b.origin.y + 13.0, b.size.width - 4.0,
                      b.size.height - 13.0 - 14.0);
}

- (double)yForQuality:(double)q {
    const NSRect r = [self plotRect];
    double t = (q - _minQ) / (_maxQ - _minQ);
    t = std::min(1.0, std::max(0.0, t));
    return r.origin.y + t * r.size.height;
}

- (double)qualityForY:(double)y {
    const NSRect r = [self plotRect];
    if (r.size.height <= 0.0) return _minQ;
    double t = (y - r.origin.y) / r.size.height;
    t = std::min(1.0, std::max(0.0, t));
    return _minQ + t * (_maxQ - _minQ);
}

// 表示順での位置 k のフレーム番号。
- (int)frameAtPosition:(int)k {
    if (_sortedByQuality && k < static_cast<int>(_qualityOrder.size())) {
        return _qualityOrder[static_cast<std::size_t>(k)];
    }
    return k;
}

- (int)positionOfFrame:(int)frame {
    if (!_sortedByQuality) return frame;
    for (std::size_t k = 0; k < _qualityOrder.size(); ++k) {
        if (_qualityOrder[k] == frame) return static_cast<int>(k);
    }
    return -1;
}

// ビューの x に対応する表示順の位置。
- (int)positionForX:(double)x {
    const NSRect r = [self plotRect];
    const int n = static_cast<int>(_quality.size());
    if (n == 0 || r.size.width <= 0.0) return -1;
    int k = static_cast<int>((x - r.origin.x) / r.size.width * n);
    return std::min(n - 1, std::max(0, k));
}

- (double)xForPosition:(int)k {
    const NSRect r = [self plotRect];
    const int n = static_cast<int>(_quality.size());
    return r.origin.x + (k + 0.5) * r.size.width / std::max(1, n);
}

// 採用フレームのうち上位何%か（除外なら負）。
- (double)topPercentOfFrame:(int)frame {
    if (frame < 0 || frame >= static_cast<int>(_ok.size()) || !_ok[static_cast<std::size_t>(frame)]) {
        return -1.0;
    }
    int rank = 0;
    for (std::size_t k = 0; k < _qualityOrder.size(); ++k) {
        if (_qualityOrder[k] == frame) {
            rank = static_cast<int>(k) + 1;
            break;
        }
    }
    return 100.0 * rank / std::max<std::size_t>(1, _sortedAccepted.size());
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    const NSRect bounds = [self bounds];
    [[NSColor controlBackgroundColor] setFill];
    NSRectFill(bounds);
    // separatorColor は10.14以降なので使わない（gridColorは10.0から）。
    [[NSColor gridColor] setStroke];
    NSFrameRect(bounds);

    NSDictionary* small = @{
        NSForegroundColorAttributeName : [NSColor secondaryLabelColor],
        NSFontAttributeName : [NSFont systemFontOfSize:9.0]
    };

    if (_quality.empty()) {
        NSMutableParagraphStyle* style = [[[NSMutableParagraphStyle alloc] init] autorelease];
        [style setAlignment:NSTextAlignmentCenter];
        NSDictionary* attrs = @{
            NSForegroundColorAttributeName : [NSColor secondaryLabelColor],
            NSFontAttributeName : [NSFont systemFontOfSize:11.0],
            NSParagraphStyleAttributeName : style
        };
        [LSLocalizedString(@"品質評価するとここに品質が出ます")
            drawInRect:NSMakeRect(bounds.origin.x, NSMidY(bounds) - 8.0, bounds.size.width, 16.0)
        withAttributes:attrs];
        return;
    }

    const NSRect r = [self plotRect];
    const int n = static_cast<int>(_quality.size());
    const double cut = [self cutQuality];

    NSColor* aboveColor = [NSColor systemBlueColor];
    NSColor* belowColor = [NSColor tertiaryLabelColor];
    NSColor* rejectColor = [NSColor systemRedColor];

    // 目盛り（最小・中央・最大の品質値）。
    [[NSColor gridColor] setStroke];
    for (int i = 0; i <= 2; ++i) {
        const double y = r.origin.y + r.size.height * i / 2.0;
        NSBezierPath* grid = [NSBezierPath bezierPath];
        [grid moveToPoint:NSMakePoint(r.origin.x, y)];
        [grid lineToPoint:NSMakePoint(NSMaxX(r), y)];
        [grid setLineWidth:0.5];
        [grid stroke];
    }
    [[NSString stringWithFormat:@"%.3g", _maxQ] drawAtPoint:NSMakePoint(r.origin.x + 2.0, NSMaxY(r) - 11.0)
                                             withAttributes:small];
    [[NSString stringWithFormat:@"%.3g", _minQ] drawAtPoint:NSMakePoint(r.origin.x + 2.0, r.origin.y + 1.0)
                                             withAttributes:small];
    // 横軸の端の値。
    NSString* left = _sortedByQuality ? LSLocalizedString(@"高品質") : @"1";
    NSString* right = _sortedByQuality
                          ? LSLocalizedString(@"低品質")
                          : [NSString stringWithFormat:@"%d", n + _displayOffset];
    if (!_sortedByQuality && _displayOffset > 0) {
        left = [NSString stringWithFormat:@"%d", _displayOffset + 1];
    }
    [left drawAtPoint:NSMakePoint(r.origin.x, bounds.origin.y + 1.0) withAttributes:small];
    const NSSize rsize = [right sizeWithAttributes:small];
    [right drawAtPoint:NSMakePoint(NSMaxX(r) - rsize.width, bounds.origin.y + 1.0) withAttributes:small];

    if (n * 3 <= static_cast<int>(r.size.width)) {
        // フレームが少ないときは1枚ずつ棒と点で描く。
        // 範囲の縦棒にすると、1枚だけの列は高さ0の横線になって読めない。
        for (int k = 0; k < n; ++k) {
            const int idx = [self frameAtPosition:k];
            const double x = [self xForPosition:k];
            if (!_ok[static_cast<std::size_t>(idx)]) {
                [rejectColor setFill];
                NSRectFill(NSMakeRect(x - 1.5, r.origin.y, 3.0, 3.0));
                continue;
            }
            const double v = _quality[static_cast<std::size_t>(idx)];
            const double y = [self yForQuality:v];
            NSColor* color = v >= cut ? aboveColor : belowColor;
            [color setFill];
            NSRectFill(NSMakeRect(x - 0.5, r.origin.y, 1.0, y - r.origin.y));
            [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(x - 2.5, y - 2.5, 5.0, 5.0)] fill];
        }
    } else {
        const int columns = std::max(1, std::min(n, static_cast<int>(r.size.width)));
        const double colWidth = r.size.width / columns;
        // 1列にフレームが何枚も入るので、最小〜最大の範囲を縦棒で描く。
        // 平均だけだと、雲が一瞬かかったような落ち込みが均されて見えなくなる。
        for (int c = 0; c < columns; ++c) {
            const int from = static_cast<int>(static_cast<double>(c) * n / columns);
            int to = static_cast<int>(static_cast<double>(c + 1) * n / columns);
            if (to <= from) to = from + 1;
            if (to > n) to = n;

            double lo = 0.0, hi = 0.0;
            bool any = false, anyAbove = false, anyRejected = false;
            for (int k = from; k < to; ++k) {
                const int idx = [self frameAtPosition:k];
                if (!_ok[static_cast<std::size_t>(idx)]) {
                    anyRejected = true;
                    continue;
                }
                const double v = _quality[static_cast<std::size_t>(idx)];
                if (!any) {
                    lo = hi = v;
                    any = true;
                } else {
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
                if (v >= cut) anyAbove = true;
            }

            const double x = r.origin.x + c * colWidth;
            if (any) {
                const double y0 = [self yForQuality:lo];
                const double y1 = [self yForQuality:hi];
                [(anyAbove ? aboveColor : belowColor) setFill];
                NSRectFill(NSMakeRect(x, y0 - 0.5, std::max(1.0, colWidth), std::max(1.5, y1 - y0 + 1.0)));
            }
            if (anyRejected) {
                // 除外フレームは下端の赤い目印で示す。
                // グラフから消してしまうと「何枚落ちたか」が分からない。
                [rejectColor setFill];
                NSRectFill(NSMakeRect(x, r.origin.y, std::max(1.0, colWidth), 2.0));
            }
        }
    }

    // いま表示しているフレーム。
    if (_currentFrame >= 0 && _currentFrame < n) {
        const int k = [self positionOfFrame:_currentFrame];
        if (k >= 0) {
            const double x = [self xForPosition:k];
            [[NSColor systemGreenColor] setStroke];
            NSBezierPath* marker = [NSBezierPath bezierPath];
            [marker moveToPoint:NSMakePoint(x, r.origin.y)];
            [marker lineToPoint:NSMakePoint(x, NSMaxY(r))];
            [marker setLineWidth:1.0];
            [marker stroke];
        }
    }

    // カットライン。
    const double cutY = [self yForQuality:cut];
    [[NSColor systemOrangeColor] setStroke];
    NSBezierPath* line = [NSBezierPath bezierPath];
    [line moveToPoint:NSMakePoint(r.origin.x, cutY)];
    [line lineToPoint:NSMakePoint(NSMaxX(r), cutY)];
    [line setLineWidth:1.5];
    [line stroke];

    NSString* label = [NSString stringWithFormat:LSLocalizedString(@"%@ 上位 %.0f%%"),
                                                 _cutLabel ? _cutLabel : @"", _cutPercent];
    NSDictionary* attrs = @{
        NSForegroundColorAttributeName : [NSColor systemOrangeColor],
        NSFontAttributeName : [NSFont systemFontOfSize:10.0]
    };
    const NSSize size = [label sizeWithAttributes:attrs];
    double labelY = cutY + 1.0;
    if (labelY + size.height > NSMaxY(r)) labelY = cutY - size.height - 1.0;
    [label drawAtPoint:NSMakePoint(NSMaxX(r) - size.width - 3.0, labelY) withAttributes:attrs];

    // カーソル位置のフレームの説明（上端）。
    if (_hoverFrame >= 0 && _hoverFrame < n) {
        NSString* text;
        const double top = [self topPercentOfFrame:_hoverFrame];
        if (top < 0.0) {
            text = [NSString stringWithFormat:LSLocalizedString(@"#%d 除外"),
                                              _hoverFrame + 1 + _displayOffset];
        } else {
            text = [NSString stringWithFormat:LSLocalizedString(@"#%d 品質 %.4g 上位 %.1f%%"),
                                              _hoverFrame + 1 + _displayOffset,
                                              _quality[static_cast<std::size_t>(_hoverFrame)], top];
        }
        [text drawAtPoint:NSMakePoint(r.origin.x + 2.0, NSMaxY(bounds) - 13.0) withAttributes:small];
    }
}

// ---- 操作 ------------------------------------------------------------------

- (void)mouseDown:(NSEvent*)event {
    if (_sortedAccepted.empty()) return;
    const NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    const double cutY = [self yForQuality:[self cutQuality]];
    // 線の近くならカットラインを動かす。離れていればそのフレームを表示する。
    if (std::fabs(p.y - cutY) <= 6.0) {
        _dragging = YES;
        [self updateCutFromEvent:event];
        return;
    }
    const int k = [self positionForX:p.x];
    if (k >= 0 && [_delegate respondsToSelector:@selector(qualityGraphView:didSelectFrame:)]) {
        [_delegate qualityGraphView:self didSelectFrame:[self frameAtPosition:k]];
    }
}

- (void)mouseDragged:(NSEvent*)event {
    if (_dragging) {
        [self updateCutFromEvent:event];
        return;
    }
    // 線以外をドラッグしたらフレームを連続して送る。
    const NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    const int k = [self positionForX:p.x];
    if (k >= 0 && [_delegate respondsToSelector:@selector(qualityGraphView:didSelectFrame:)]) {
        [_delegate qualityGraphView:self didSelectFrame:[self frameAtPosition:k]];
    }
}

- (void)mouseUp:(NSEvent*)event {
    (void)event;
    _dragging = NO;
}

- (void)mouseMoved:(NSEvent*)event {
    if (_quality.empty()) return;
    const NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    const int k = [self positionForX:p.x];
    const int frame = k >= 0 ? [self frameAtPosition:k] : -1;
    if (frame != _hoverFrame) {
        _hoverFrame = frame;
        [self setNeedsDisplay:YES];
    }
}

- (void)mouseExited:(NSEvent*)event {
    (void)event;
    _hoverFrame = -1;
    [self setNeedsDisplay:YES];
}

- (void)updateCutFromEvent:(NSEvent*)event {
    const NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    const double q = [self qualityForY:p.y];

    // 品質の値から選択率へ戻す。「その値以上の採用フレームが何枚あるか」を数える。
    int count = 0;
    for (std::size_t i = 0; i < _sortedAccepted.size(); ++i) {
        if (_sortedAccepted[i] >= q) ++count;
    }
    if (count < 1) count = 1;
    const double percent = 100.0 * count / static_cast<double>(_sortedAccepted.size());

    [self setCutPercent:percent];
    if (_delegate) [_delegate qualityGraphView:self didChangeCutPercent:_cutPercent];
}

@end
