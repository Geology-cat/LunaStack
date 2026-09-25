#import "QueueItem.h"

#import "Localization.h"

@implementation QueueItem

@synthesize path = _path;
@synthesize state = _state;
@synthesize subtitle = _subtitle;
@synthesize message = _message;
@synthesize sequenceFiles = _sequenceFiles;
@synthesize isSequence = _isSequence;

+ (instancetype)itemWithPath:(NSString*)path {
    QueueItem* item = [[[QueueItem alloc] init] autorelease];
    [item setPath:path];
    [item setState:QueueItemStatePending];
    [item setSubtitle:@""];
    [item setMessage:@""];
    [item setSequenceFiles:@[]];
    [item setIsSequence:NO];
    return item;
}

+ (instancetype)sequenceItemWithDirectory:(NSString*)directory files:(NSArray*)files {
    QueueItem* item = [self itemWithPath:directory];
    [item setSequenceFiles:files ? files : @[]];
    [item setIsSequence:YES];
    return item;
}

- (NSString*)displayName {
    if (!_isSequence) return [_path lastPathComponent];
    if ([_sequenceFiles count] > 0) {
        return [NSString stringWithFormat:LSLocalizedString(@"%@ ほか（静止画 %lu枚）"),
                                          [[_sequenceFiles firstObject] lastPathComponent],
                                          (unsigned long)[_sequenceFiles count]];
    }
    return [NSString stringWithFormat:LSLocalizedString(@"%@（静止画連番）"),
                                      [_path lastPathComponent]];
}

- (BOOL)isSameInputAs:(QueueItem*)other {
    if (_isSequence != [other isSequence]) return NO;
    if (![_path isEqualToString:[other path]]) return NO;
    return [_sequenceFiles isEqualToArray:[other sequenceFiles]];
}

- (void)dealloc {
    [_path release];
    [_subtitle release];
    [_message release];
    [_sequenceFiles release];
    [super dealloc];
}

- (NSString*)stateSymbol {
    switch (_state) {
        case QueueItemStateQualityEvaluated:
            return LSLocalizedString(@"品質評価済");
        case QueueItemStateAnalyzed:
            return LSLocalizedString(@"アライメント済");
        case QueueItemStateStacked:
            return LSLocalizedString(@"完了");
        case QueueItemStateError:
            return LSLocalizedString(@"失敗");
        case QueueItemStatePending:
        default:
            return LSLocalizedString(@"未処理");
    }
}

- (NSColor*)stateColor {
    switch (_state) {
        case QueueItemStateQualityEvaluated:
            return [NSColor systemBlueColor];
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
