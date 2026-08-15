#import "QueueItem.h"

@implementation QueueItem

@synthesize path = _path;
@synthesize state = _state;
@synthesize subtitle = _subtitle;
@synthesize message = _message;

+ (instancetype)itemWithPath:(NSString*)path {
    QueueItem* item = [[[QueueItem alloc] init] autorelease];
    [item setPath:path];
    [item setState:QueueItemStatePending];
    [item setSubtitle:@""];
    [item setMessage:@""];
    return item;
}

- (void)dealloc {
    [_path release];
    [_subtitle release];
    [_message release];
    [super dealloc];
}

- (NSString*)stateSymbol {
    switch (_state) {
        case QueueItemStateAnalyzed:
            return @"解析済";
        case QueueItemStateStacked:
            return @"完了";
        case QueueItemStateError:
            return @"失敗";
        case QueueItemStatePending:
        default:
            return @"未処理";
    }
}

- (NSColor*)stateColor {
    switch (_state) {
        case QueueItemStateAnalyzed:
            return [NSColor systemBlueColor];
        case QueueItemStateStacked:
            return [NSColor systemGreenColor];
        case QueueItemStateError:
            return [NSColor systemRedColor];
        case QueueItemStatePending:
        default:
            return [NSColor secondaryLabelColor];
    }
}

@end
