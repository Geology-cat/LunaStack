#import "QualityGraphView.h"

#include <algorithm>
#include <cmath>
#include <vector>

@implementation QualityGraphView {
    std::vector<double> _quality;    // フレーム番号順
    std::vector<unsigned char> _ok;  // 採用されたか
    // 採用フレームの品質を降順に並べたもの。カットラインの換算に使う。
    std::vector<double> _sortedAccepted;
    double _minQ;
    double _maxQ;
    BOOL _dragging;
}

@synthesize cutPercent = _cutPercent;
@synthesize sortedByQuality = _sortedByQuality;
@synthesize delegate = _delegate;

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        _cutPercent = 25.0;
        _sortedByQuality = NO;
        _minQ = 0.0;
        _maxQ = 1.0;
        _dragging = NO;
    }
    return self;
}

- (BOOL)hasData {
    return !_quality.empty();
}

- (void)clearData {
    _quality.clear();
    _ok.clear();
    _sortedAccepted.clear();
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

- (void)setSortedByQuality:(BOOL)sorted {
    _sortedByQuality = sorted;
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
    return NSInsetRect(b, 2.0, 2.0);
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

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    const NSRect bounds = [self bounds];
    [[NSColor controlBackgroundColor] setFill];
    NSRectFill(bounds);
    // separatorColor は10.14以降なので使わない（gridColorは10.0から）。
    [[NSColor gridColor] setStroke];
    NSFrameRect(bounds);

    if (_quality.empty()) {
        NSMutableParagraphStyle* style = [[[NSMutableParagraphStyle alloc] init] autorelease];
        [style setAlignment:NSTextAlignmentCenter];
        NSDictionary* attrs = @{
            NSForegroundColorAttributeName : [NSColor secondaryLabelColor],
            NSFontAttributeName : [NSFont systemFontOfSize:11.0],
            NSParagraphStyleAttributeName : style
        };
        [@"解析するとここに品質が出ます"
            drawInRect:NSMakeRect(bounds.origin.x, NSMidY(bounds) - 8.0, bounds.size.width, 16.0)
        withAttributes:attrs];
        return;
    }

    const NSRect r = [self plotRect];
    const int n = static_cast<int>(_quality.size());
    const int columns = std::max(1, std::min(n, static_cast<int>(r.size.width)));
    const double colWidth = r.size.width / columns;
    const double cut = [self cutQuality];

    // 表示順。品質順のときは降順に並べ替えたインデックス列を使う。
    std::vector<int> order(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) order[static_cast<std::size_t>(i)] = i;
    if (_sortedByQuality) {
        const std::vector<double>& q = _quality;
        std::sort(order.begin(), order.end(), [&q](int a, int b) {
            if (q[static_cast<std::size_t>(a)] != q[static_cast<std::size_t>(b)]) {
                return q[static_cast<std::size_t>(a)] > q[static_cast<std::size_t>(b)];
            }
            return a < b;
        });
    }

    NSColor* aboveColor = [NSColor systemBlueColor];
    NSColor* belowColor = [NSColor tertiaryLabelColor];
    NSColor* rejectColor = [NSColor systemRedColor];

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
            const int idx = order[static_cast<std::size_t>(k)];
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
            NSRectFill(NSMakeRect(x, y0, std::max(1.0, colWidth), std::max(1.0, y1 - y0)));
        }
        if (anyRejected) {
            // 除外フレームは下端の赤い目印で示す。
            // グラフから消してしまうと「何枚落ちたか」が分からない。
            [rejectColor setFill];
            NSRectFill(NSMakeRect(x, r.origin.y, std::max(1.0, colWidth), 2.0));
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

    NSString* label = [NSString stringWithFormat:@"上位 %.0f%%", _cutPercent];
    NSDictionary* attrs = @{
        NSForegroundColorAttributeName : [NSColor systemOrangeColor],
        NSFontAttributeName : [NSFont systemFontOfSize:10.0]
    };
    const NSSize size = [label sizeWithAttributes:attrs];
    double labelY = cutY + 1.0;
    if (labelY + size.height > NSMaxY(r)) labelY = cutY - size.height - 1.0;
    [label drawAtPoint:NSMakePoint(NSMaxX(r) - size.width - 3.0, labelY) withAttributes:attrs];
}

// ---- カットラインのドラッグ ------------------------------------------------

- (void)mouseDown:(NSEvent*)event {
    if (_sortedAccepted.empty()) return;
    _dragging = YES;
    [self updateCutFromEvent:event];
}

- (void)mouseDragged:(NSEvent*)event {
    if (!_dragging) return;
    [self updateCutFromEvent:event];
}

- (void)mouseUp:(NSEvent*)event {
    (void)event;
    _dragging = NO;
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
