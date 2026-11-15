#import "PreferencesWindowController.h"

#import "Localization.h"

static NSString* const kLanguagesKey = @"AppleLanguages";

@implementation PreferencesWindowController {
    NSPopUpButton* _languagePopup;
    NSTextField* _noteLabel;
    NSButton* _relaunchButton;
    // 起動したときの表示言語。選び直した言語がこれと違う間だけ、再起動を促す。
    NSString* _launchLanguage;
}

+ (NSString*)storedLanguage {
    // standardUserDefaults から直接読むと、アプリに保存が無いときにシステム全体の
    // 言語の並びが返ってきて区別できない。アプリ自身の領域だけを見る。
    NSString* bundleID = [[NSBundle mainBundle] bundleIdentifier];
    if (!bundleID) return nil;
    NSDictionary* domain = [[NSUserDefaults standardUserDefaults] persistentDomainForName:bundleID];
    id value = [domain objectForKey:kLanguagesKey];
    if (![value isKindOfClass:[NSArray class]] || [value count] == 0) return nil;
    id first = [value objectAtIndex:0];
    if (![first isKindOfClass:[NSString class]]) return nil;
    if ([first hasPrefix:@"ja"]) return @"ja";
    if ([first hasPrefix:@"en"]) return @"en";
    return nil;
}

+ (void)setStoredLanguage:(NSString*)language {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    if (language) {
        [defaults setObject:@[ language ] forKey:kLanguagesKey];
    } else {
        [defaults removeObjectForKey:kLanguagesKey];
    }
    [defaults synchronize];
}

- (instancetype)init {
    NSWindow* window =
        [[[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 440, 150)
                                     styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                       backing:NSBackingStoreBuffered
                                         defer:YES] autorelease];
    self = [super initWithWindow:window];
    if (self) {
        _launchLanguage = [[PreferencesWindowController storedLanguage] copy];
        [window setTitle:LSLocalizedString(@"環境設定")];
        [window setReleasedWhenClosed:NO];
        [self buildContent];
        [window center];
    }
    return self;
}

- (void)dealloc {
    [_launchLanguage release];
    [super dealloc];
}

- (void)buildContent {
    NSTextField* label = [NSTextField labelWithString:LSLocalizedString(@"表示言語:")];

    // 言語の名前は、どちらの表示でも読めるようにその言語で書く（翻訳しない）。
    _languagePopup = [[[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO] autorelease];
    [_languagePopup addItemWithTitle:LSLocalizedString(@"システムの設定に合わせる")];
    [_languagePopup addItemWithTitle:@"日本語"];
    [_languagePopup addItemWithTitle:@"English"];
    [[_languagePopup itemAtIndex:1] setRepresentedObject:@"ja"];
    [[_languagePopup itemAtIndex:2] setRepresentedObject:@"en"];
    [_languagePopup setTarget:self];
    [_languagePopup setAction:@selector(languageChanged:)];

    NSStackView* row = [NSStackView stackViewWithViews:@[ label, _languagePopup ]];
    [row setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
    [row setSpacing:8];

    _noteLabel = [NSTextField wrappingLabelWithString:@""];
    [_noteLabel setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [_noteLabel setTextColor:[NSColor secondaryLabelColor]];
    [_noteLabel setPreferredMaxLayoutWidth:400];

    _relaunchButton = [NSButton buttonWithTitle:LSLocalizedString(@"今すぐ再起動")
                                         target:self
                                         action:@selector(relaunch:)];

    NSStackView* stack = [NSStackView stackViewWithViews:@[ row, _noteLabel, _relaunchButton ]];
    [stack setOrientation:NSUserInterfaceLayoutOrientationVertical];
    [stack setAlignment:NSLayoutAttributeLeading];
    [stack setSpacing:12];
    [stack setEdgeInsets:NSEdgeInsetsMake(20, 20, 20, 20)];
    [stack setTranslatesAutoresizingMaskIntoConstraints:NO];

    NSView* content = [[self window] contentView];
    [content addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [[stack leadingAnchor] constraintEqualToAnchor:[content leadingAnchor]],
        [[stack trailingAnchor] constraintEqualToAnchor:[content trailingAnchor]],
        [[stack topAnchor] constraintEqualToAnchor:[content topAnchor]],
        [[stack bottomAnchor] constraintEqualToAnchor:[content bottomAnchor]],
        [[stack widthAnchor] constraintEqualToConstant:440],
    ]];

    [self refresh];
}

// 保存してある言語を画面に映す。
- (void)refresh {
    NSString* stored = [PreferencesWindowController storedLanguage];
    NSInteger index = 0;
    if ([stored isEqualToString:@"ja"]) index = 1;
    else if ([stored isEqualToString:@"en"]) index = 2;
    [_languagePopup selectItemAtIndex:index];

    const BOOL changed = !(stored == _launchLanguage || [stored isEqualToString:_launchLanguage]);
    [_noteLabel setStringValue:LSLocalizedString(
                                   changed ? @"表示言語の変更は、LunaStack を次に起動したときから反映されます。"
                                           : @"表示言語を変えると、LunaStack を次に起動したときから反映されます。")];
    [_relaunchButton setEnabled:changed];
}

- (void)languageChanged:(id)sender {
    (void)sender;
    [PreferencesWindowController setStoredLanguage:[[_languagePopup selectedItem] representedObject]];
    [self refresh];
}

// いったん終了して起動し直す。
//
// 終了を見届けてから開き直す役を、小さなシェルに任せる（自分では自分を起動し直せない）。
// 処理中で終了の確認を取り消したときなど、終了しなかった場合は何もしない（1分で諦める）。
- (void)relaunch:(id)sender {
    (void)sender;
    NSString* script = @"i=0; while kill -0 \"$1\" 2>/dev/null && [ $i -lt 600 ]; do "
                       @"sleep 0.1; i=$((i+1)); done; "
                       @"kill -0 \"$1\" 2>/dev/null || /usr/bin/open \"$0\"";
    NSTask* task = [[[NSTask alloc] init] autorelease];
    [task setLaunchPath:@"/bin/sh"];
    [task setArguments:@[
        @"-c", script, [[NSBundle mainBundle] bundlePath],
        [NSString stringWithFormat:@"%d", [[NSProcessInfo processInfo] processIdentifier]]
    ]];
    @try {
        [task launch];
    } @catch (NSException* e) {
        NSLog(@"再起動の準備に失敗しました: %@", e);
        return;
    }
    [NSApp terminate:nil];
}

@end
