#import "MainWindowController_Private.h"

#include <algorithm>
#include <cmath>

#include "stackcore/map_pipeline.hpp"
#include "stackcore/video_source.hpp"
#include "stackcore/wavelet.hpp"

@implementation MainWindowController (SelfCheck)

// APの当たり判定を自己検証する。
//
// **描画の座標変換と当たり判定の座標変換がずれる**のが、この手のUIで
// いちばん気づきにくい壊れ方である。枠は正しく見えているのに
// クリックすると別のAPが選ばれる、という形で出る。
// 画面に描いた位置をそのまま押したとき、同じAPが返るかを全点で確かめる。
- (BOOL)selfCheckApHitTest {
    if (!_analysis || _analysis->points.empty()) {
        NSLog(@"AP当たり判定の自己検証: APが無いので確認できません");
        return NO;
    }
    const double coordScale = _stacked ? LSDrizzleScaleAt([_drizzleSegment selectedSegment]) : 1.0;

    int checked = 0, mismatched = 0;
    double maxRoundTrip = 0.0;
    for (std::size_t i = 0; i < _analysis->points.size(); ++i) {
        const NSPoint img = NSMakePoint(_analysis->points[i].cx * coordScale,
                                        _analysis->points[i].cy * coordScale);
        const NSPoint view = [_preview viewPointFromImagePoint:img];
        const NSPoint back = [_preview imagePointFromViewPoint:view];
        maxRoundTrip = std::max(maxRoundTrip,
                                std::max(std::fabs(back.x - img.x), std::fabs(back.y - img.y)));

        const NSInteger hit = [_preview apIndexAtViewPoint:view];
        if (hit != static_cast<NSInteger>(i)) ++mismatched;
        ++checked;
    }
    NSLog(@"AP当たり判定の自己検証: %d点中 一致しない %d点 / 往復誤差 最大 %.4f px", checked,
          mismatched, maxRoundTrip);
    return mismatched == 0 && maxRoundTrip < 0.01;
}

// AP編集とプリセットの往復を自己検証する。
//
// どちらも「画面のつまみ → 設定 → 保存 → 復元」の受け渡しであり、
// 途中の1項目を書き忘れても画面上は何も起きない。気づくのは
// 「プリセットを読んだのに前と結果が違う」という遠い場所になる。
- (BOOL)selfCheckEditingAndPresets {
    BOOL ok = YES;

    // --- AP編集 ---
    if (_analysis && !_analysis->points.empty()) {
        const std::size_t before = _analysis->points.size();
        NSString* signatureBefore = [[self analysisSignature] copy];

        [self previewView:_preview didDeleteApAtIndex:0];
        [self previewView:_preview didAddApAtX:_analysis ? 0 : 10 y:10];

        const stackcore::MapStackSettings settings = [self currentSettings];
        if (!settings.ap.use_manual_points) {
            NSLog(@"AP編集の自己検証: 手動配置が有効になっていません");
            ok = NO;
        }
        if (settings.ap.manual_points.size() != before) {
            // 1つ消して1つ足したので数は変わらない
            NSLog(@"AP編集の自己検証: AP数が合いません（%zu → %zu、期待 %zu）", before,
                  settings.ap.manual_points.size(), before);
            ok = NO;
        }
        if ([signatureBefore isEqualToString:[self analysisSignature]]) {
            NSLog(@"AP編集の自己検証: APを変えたのに解析が無効になっていません");
            ok = NO;
        }
        [signatureBefore release];

        // 元に戻す
        _manualPointsActive = NO;
        _manualPoints.clear();
    }

    // --- プリセット ---
    NSDictionary* original = [self settingsDictionary];
    [_apTopSlider setDoubleValue:37.0];
    [_topSlider setDoubleValue:41.0];
    [_sharpenSliders[2] setDoubleValue:2.25];
    [_denoiseSliders[4] setDoubleValue:0.55];
    [_drizzleSegment setSelectedSegment:2];
    [_endianPopup selectItemAtIndex:2];
    NSDictionary* modified = [self settingsDictionary];

    NSString* error = nil;
    NSString* name = @"__self_check__";
    if (![Presets saveSettings:modified name:name error:&error]) {
        NSLog(@"プリセットの自己検証: 保存に失敗 %@", error);
        ok = NO;
    } else {
        [self applySettingsDictionary:original];
        NSDictionary* loaded = [Presets loadSettingsNamed:name];
        [self applySettingsDictionary:loaded];
        NSDictionary* restored = [self settingsDictionary];
        for (NSString* key in modified) {
            if (![[restored[key] description] isEqualToString:[modified[key] description]]) {
                NSLog(@"プリセットの自己検証: %@ が戻りません（%@ → %@）", key, modified[key],
                      restored[key]);
                ok = NO;
            }
        }
        [Presets removeSettingsNamed:name];
        [self applySettingsDictionary:original];
    }

    NSLog(@"AP編集・プリセットの自己検証: %@", ok ? @"問題なし" : @"問題あり");
    return ok;
}

@end
