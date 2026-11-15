#pragma once

#import <Cocoa/Cocoa.h>

// 環境設定のウインドウ。いまは表示言語だけを持つ。
//
// 表示言語は、アプリ自身の設定領域の AppleLanguages に書く。これは macOS が
// 起動時に読む標準の場所なので、自前の文字列だけでなく、開く／保存のパネルや
// 「LunaStack について」などシステムが出す部品も同じ言語になる。
// 起動時に決まるものなので、変更は次の起動から効く。
@interface PreferencesWindowController : NSWindowController

// アプリに保存してある表示言語（@"ja" / @"en"）。システムに合わせるなら nil。
+ (NSString*)storedLanguage;

// 表示言語を保存する。nil を渡すとシステムの設定に合わせる。
+ (void)setStoredLanguage:(NSString*)language;

@end
