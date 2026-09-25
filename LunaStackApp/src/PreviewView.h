#pragma once

#import <Cocoa/Cocoa.h>

#include <memory>
#include <vector>

#include "stackcore/ap_placer.hpp"
#include "stackcore/frame_buffer.hpp"

@class PreviewView;

@protocol PreviewViewDelegate <NSObject>
// 画像座標 (x, y) にAPを追加したい。
- (void)previewView:(PreviewView*)view didAddApAtX:(int)x y:(int)y;
// index 番目のAPを削除したい。
- (void)previewView:(PreviewView*)view didDeleteApAtIndex:(NSInteger)index;
@optional
// カーソル位置の座標と画素値の説明。画像の外では nil。
- (void)previewView:(PreviewView*)view hoverDescription:(NSString*)text;
// ←→キーでフレームを送りたい（Shiftで10枚、Optionで100枚）。
- (void)previewView:(PreviewView*)view didRequestFrameStep:(int)step;
// ホイールやピンチで倍率が変わった（ツールバーの表示を合わせるため）。
- (void)previewViewZoomDidChange:(PreviewView*)view;
@end

// スタック結果を表示するビュー。
//
// NSImageView を使わず自前で描くのは、拡大時の補間を自分で決めたいため。
// 天体画像は等倍以上に拡大して細部を見る使い方をするので、
// 拡大時に滑らかに補間されると「実際に写っているもの」が分からなくなる。
// 等倍以上では最近傍で描く。
@interface PreviewView : NSView

// 表示する画像を差し替える。0..1 正規化された FrameBuffer を受け取る。
//
// **`setFrame:` という名前にしてはいけない。** NSView が NSRect を取る
// 同名のメソッドを持っており、AppKitがレイアウト時にそれを呼ぶと
// NSRect を FrameBuffer の参照として解釈して即クラッシュする。
// Objective-C のセレクタは引数の型を含まないので、コンパイラは警告を出さない。
- (void)showFrameBuffer:(const stackcore::FrameBuffer&)frame;
// 共有して表示する（複製しない）。大きなスタック結果ではこちらを使う。
- (void)showSharedFrame:(std::shared_ptr<const stackcore::FrameBuffer>)frame;
- (void)clearImage;
- (BOOL)hasImage;
- (int)imageWidth;
- (int)imageHeight;

// 表示倍率。0 で「ウィンドウに合わせる」（小さな画像は拡大して合わせる）。
// それ以外は**画面の実画素**に対する倍率。1 なら画像1画素＝画面1画素
// （Retinaでも本当の等倍になる）。
@property(nonatomic) double zoom;
// いまの実効倍率（画面の実画素に対する）。「全体」表示のときも値を返す。
- (double)effectiveDeviceZoom;
- (void)zoomInStep;
- (void)zoomOutStep;

// 表示だけを明るくする（自動ストレッチ）。
//
// 惑星のスタック結果は最大値が0.3程度にしかならないことが多い。
// 素の値をそのまま8bitに落とすと、画面上では真っ暗に近くなり
// 仕上げスライダーを動かしても違いが見えない。
// **保存されるデータには一切影響しない。** 見るための変換である。
@property(nonatomic) BOOL displayStretch;

// ---- APオーバーレイ（UI設計書 §5.2） ----

// APの一覧を渡す。
//
// coordinateScale は「AP座標 → 表示中の画像の画素」の倍率。
// APは参照画像（＝入力と同じ大きさ）の座標系で持つが、Drizzleを使うと
// 表示している画像はそれより大きい。ここで吸収する。
//
// meanQualities は AP ごとの平均品質。空ならヒートマップは描かない。
- (void)setAlignmentPoints:(const std::vector<stackcore::AlignmentPoint>&)points
                    apSize:(int)apSize
             meanQualities:(const std::vector<double>&)meanQualities
           coordinateScale:(double)coordinateScale;
- (void)clearAlignmentPoints;

@property(nonatomic) BOOL showAlignmentPoints;
// APごとの平均品質を寒色→暖色で塗る。どこが最後までシーイングに負けていたかが分かる。
@property(nonatomic) BOOL apHeatmap;
// クリックでAP追加、選択して Delete で削除。
@property(nonatomic) BOOL apEditing;

// クロップ範囲の枠（画像座標）。幅0で描かない。
@property(nonatomic) NSRect cropOverlay;

@property(nonatomic, assign) id<PreviewViewDelegate> delegate;

// 画像座標 ⇄ ビュー座標。
//
// **一箇所にまとめておくこと。** 描画とヒットテストで別々に計算すると、
// 片方だけ直したときに「見えている枠と当たり判定がずれる」という
// いちばん気づきにくい壊れ方をする。
- (NSRect)imageDrawRect;
- (NSPoint)imagePointFromViewPoint:(NSPoint)p;
- (NSPoint)viewPointFromImagePoint:(NSPoint)p;

// ビュー座標にあるAPの番号。無ければ -1。
// クリック処理そのものが使う経路なので、自己検証もここを通す。
- (NSInteger)apIndexAtViewPoint:(NSPoint)p;
- (NSInteger)alignmentPointCount;

// 指定の倍率で、画像座標 p がビュー座標 anchor に来るよう表示を合わせる
// （カーソル位置を中心にしたズーム）。
- (void)setZoom:(double)zoom keepingImagePoint:(NSPoint)p atViewPoint:(NSPoint)anchor;

@end
