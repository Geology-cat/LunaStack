#pragma once

#import <Cocoa/Cocoa.h>

// 処理設定のプリセット（仕様書 §5.1）。
//
// `~/Library/Application Support/LunaStack/Presets/<名前>.json` に1件1ファイルで置く。
// 形式はJSON（`NSJSONSerialization`）。自前のパーサを書かない。
// 保存先をFinderで開いて中身を読める・手で消せることを優先している。
//
// プリセットはUIの都合なので、エンジン（libstackcore）には入れない。
@interface Presets : NSObject

+ (NSString*)directory;
// 名前の一覧（拡張子なし、五十音・アルファベット順）。
+ (NSArray*)names;

+ (BOOL)saveSettings:(NSDictionary*)settings name:(NSString*)name error:(NSString**)error;
// 見つからない・壊れている場合は nil。
+ (NSDictionary*)loadSettingsNamed:(NSString*)name;
+ (BOOL)removeSettingsNamed:(NSString*)name;

@end
