#import "Presets.h"

#import "Localization.h"

@implementation Presets

+ (NSString*)directory {
    NSArray* paths = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory,
                                                         NSUserDomainMask, YES);
    NSString* base = [paths firstObject];
    if (!base) base = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support"];
    return [base stringByAppendingPathComponent:@"LunaStack/Presets"];
}

+ (NSString*)pathForName:(NSString*)name {
    // ファイル名に使えない文字だけ置き換える。
    // 利用者が付けた名前をそのまま見出しに出したいので、
    // 安全な文字だけに削るのではなく、区切り文字だけを潰す。
    NSString* safe = [name stringByReplacingOccurrencesOfString:@"/" withString:@"-"];
    safe = [safe stringByReplacingOccurrencesOfString:@":" withString:@"-"];
    return [[[self directory] stringByAppendingPathComponent:safe]
        stringByAppendingPathExtension:@"json"];
}

+ (NSArray*)names {
    NSFileManager* fm = [NSFileManager defaultManager];
    NSArray* files = [fm contentsOfDirectoryAtPath:[self directory] error:NULL];
    NSMutableArray* out = [NSMutableArray array];
    for (NSString* f in files) {
        if ([[f pathExtension] isEqualToString:@"json"]) {
            [out addObject:[f stringByDeletingPathExtension]];
        }
    }
    [out sortUsingSelector:@selector(localizedCaseInsensitiveCompare:)];
    return out;
}

+ (BOOL)saveSettings:(NSDictionary*)settings name:(NSString*)name error:(NSString**)error {
    if ([name length] == 0) {
        if (error) *error = LSLocalizedString(@"名前が空です");
        return NO;
    }
    NSFileManager* fm = [NSFileManager defaultManager];
    NSError* err = nil;
    if (![fm createDirectoryAtPath:[self directory]
       withIntermediateDirectories:YES
                        attributes:nil
                             error:&err]) {
        if (error) *error = [err localizedDescription];
        return NO;
    }
    NSData* data = [NSJSONSerialization dataWithJSONObject:settings
                                                   options:NSJSONWritingPrettyPrinted
                                                     error:&err];
    if (!data) {
        if (error) *error = [err localizedDescription];
        return NO;
    }
    if (![data writeToFile:[self pathForName:name] atomically:YES]) {
        if (error) *error = LSLocalizedString(@"書き込めませんでした");
        return NO;
    }
    return YES;
}

+ (NSDictionary*)loadSettingsNamed:(NSString*)name {
    NSData* data = [NSData dataWithContentsOfFile:[self pathForName:name]];
    if (!data) return nil;
    id obj = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
    if (![obj isKindOfClass:[NSDictionary class]]) return nil;
    return obj;
}

+ (BOOL)removeSettingsNamed:(NSString*)name {
    return [[NSFileManager defaultManager] removeItemAtPath:[self pathForName:name] error:NULL];
}

@end
