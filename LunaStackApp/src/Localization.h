#pragma once

#import <Cocoa/Cocoa.h>

// 日本語の表示文をキーとして使う。翻訳が無い場合はキー自身へ戻るため、
// 既存の日本語UIを保ったまま段階的に英語化できる。
static inline NSString* LSLocalizedString(NSString* key) {
    if (!key || [key length] == 0) return key;
    return [[NSBundle mainBundle] localizedStringForKey:key value:key table:nil];
}

// コードで組み立てた初期UIを一括で翻訳する。
// 実行中に変化する文字列は、各更新箇所でLSLocalizedStringを使う。
static inline void LSLocalizeViewTree(NSView* view) {
    if ([view isKindOfClass:[NSSegmentedControl class]]) {
        NSSegmentedControl* control = (NSSegmentedControl*)view;
        for (NSInteger i = 0; i < [control segmentCount]; ++i) {
            NSString* label = [control labelForSegment:i];
            if (label) [control setLabel:LSLocalizedString(label) forSegment:i];
        }
    } else if ([view isKindOfClass:[NSPopUpButton class]]) {
        for (NSMenuItem* item in [(NSPopUpButton*)view itemArray]) {
            [item setTitle:LSLocalizedString([item title])];
        }
    } else if ([view isKindOfClass:[NSButton class]]) {
        NSButton* button = (NSButton*)view;
        [button setTitle:LSLocalizedString([button title])];
    } else if ([view isKindOfClass:[NSTextField class]]) {
        NSTextField* field = (NSTextField*)view;
        if (![field isEditable]) [field setStringValue:LSLocalizedString([field stringValue])];
    }

    for (NSView* child in [view subviews]) LSLocalizeViewTree(child);
}

