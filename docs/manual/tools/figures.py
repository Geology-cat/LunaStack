#!/usr/bin/env python3
"""撮った画面（build/raw）から説明書の図（figures/）を作る。

図ごとに、切り出す範囲と「印を付けたい部品」（アンカー）を決める。部品の位置はアプリが書き出した
レイアウト（部品の種類・文言・所属する設定欄・矩形）から探すので、画面が変わっても撮り直して
このスクリプトを流し直せば、番号や枠の位置は自動で追従する。

出力:
  figures/<図>.png / .jpg     切り出して縮めた画像
  figures/<図>.coords.tex     \\lsanchor{名前}{x0}{y0}{x1}{y1}（画像の左下を原点に 0〜1 で正規化）

使い方: python3 docs/manual/tools/figures.py [--list 場面名]   （--list は部品の一覧を出す。図を作るときの手がかり）
"""

import json
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
MANUAL = os.path.dirname(HERE)
RAW = os.path.join(MANUAL, "build", "raw")
FIG = os.path.join(MANUAL, "figures")


class Shot:
    """1枚の撮影（画像と部品の位置）。座標はポイント・左上原点。"""

    def __init__(self, name):
        self.name = name
        png = os.path.join(RAW, name + ".png")
        js = png[:-4] + ".json" if not name.endswith("_insp") else png + ".json"
        with open(js, encoding="utf-8") as f:
            doc = json.load(f)
        self.size = doc["size"]
        self.items = doc["items"]
        img = Image.open(png).convert("RGBA")
        # 設定パネルだけを描いた画像は背景が透明なので、ウインドウの背景色（235）に重ねる。
        bg = Image.new("RGBA", img.size, (235, 235, 235, 255))
        self.image = Image.alpha_composite(bg, img).convert("RGB")
        self.scale = self.image.size[0] / self.size[0]  # 画素 / ポイント（Retina で 2）

    def find(self, text=None, cls=None, section=None, nth=0, exact=False):
        hits = []
        for e in self.items:
            if cls and not e["class"].endswith(cls):
                continue
            if section is not None and e["section"] != section:
                continue
            if text is not None:
                t = e["text"]
                if exact and t != text:
                    continue
                if not exact and text not in t:
                    continue
            hits.append(e)
        # 上から下、左から右の順にそろえる（nth の意味を安定させる）
        hits.sort(key=lambda e: (round(e["rect"][1]), e["rect"][0]))
        if len(hits) <= nth:
            raise KeyError(f"{self.name}: 見つからない text={text!r} cls={cls!r} section={section!r} nth={nth}")
        return list(hits[nth]["rect"])

    def section_rect(self, key):
        """設定欄（見出し＋中身）全体の矩形。"""
        rects = [e["rect"] for e in self.items if e["section"] in (key, "header:" + key)]
        if not rects:
            raise KeyError(f"{self.name}: 設定欄 {key} が無い")
        return union(rects)

    def orange_line(self, graph_rect):
        """品質グラフのカットライン（オレンジの横線）の矩形。"""
        x, y, w, h = graph_rect
        s = self.scale
        best, best_row = 0, None
        for py in range(int(y * s), int((y + h) * s)):
            count = 0
            for px in range(int(x * s), int((x + w) * s), 3):
                r, g, b = self.image.getpixel((px, py))
                if r > 200 and 110 < g < 190 and b < 90:
                    count += 1
            if count > best:
                best, best_row = count, py
        if best_row is None:
            raise KeyError(f"{self.name}: カットラインが見つからない")
        return [x, best_row / s - 1.5, w, 3.0]


def union(rects):
    x0 = min(r[0] for r in rects)
    y0 = min(r[1] for r in rects)
    x1 = max(r[0] + r[2] for r in rects)
    y1 = max(r[1] + r[3] for r in rects)
    return [x0, y0, x1 - x0, y1 - y0]


def pad(r, p):
    return [r[0] - p, r[1] - p, r[2] + 2 * p, r[3] + 2 * p]


# ---- よく使う部品の探し方 --------------------------------------------------------

def btn(t, **k):
    return ("find", dict(text=t, cls="NSButton", exact=True, **k))


def lbl(t, **k):
    return ("find", dict(text=t, cls="NSTextField", **k))


def seglabel(t, nth=0):
    return ("find", dict(text=t, cls="NSSegmentItemLabelView", exact=True, nth=nth))


def cls(c, nth=0, **k):
    return ("find", dict(cls=c, nth=nth, **k))


def popup(section, nth=0):
    return ("find", dict(cls="NSPopUpButton", section=section, nth=nth))


def sec(key):
    return ("section", key)


def uni(*specs):
    return ("union", specs)


def rect(x, y, w, h):
    return ("rect", [x, y, w, h])


def resolve(shot, spec):
    kind, arg = spec
    if kind == "find":
        return shot.find(**arg)
    if kind == "section":
        return shot.section_rect(arg)
    if kind == "union":
        return union([resolve(shot, s) for s in arg])
    if kind == "rect":
        return list(arg)
    if kind == "cutline":
        return shot.orange_line(resolve(shot, arg))
    if kind == "levelmark":
        # レベル補正の三角（LevelsView: 左右 7pt の余白、下の 16pt の帯に高さ 11pt の三角）
        x, y, w, h = shot.find(cls="LevelsView")
        cx = x + 7 + arg * (w - 14)
        return [cx - 7, y + h - 15, 14, 13]
    if kind == "imgrect":
        # プレビューに全体表示した画像の上の矩形（画像の画素座標 → 画面のポイント）
        iw, ih, rx, ry, rw, rh = arg
        px, py, pw, ph = shot.find(cls="PreviewView")
        k = min(pw / iw, ph / ih)
        ox = px + (pw - iw * k) / 2
        oy = py + (ph - ih * k) / 2
        return [ox + rx * k, oy + ry * k, rw * k, rh * k]
    raise ValueError(kind)


# 画面の決まった領域（ポイント、1280×800 のウインドウ）
LEFT = [0, 0, 236, 758]
CENTER = [232, 0, 736, 740]
RIGHT = [962, 0, 318, 742]
BOTTOM = [0, 742, 1280, 58]
PREVIEW_ONLY = ("find", dict(cls="PreviewView"))

# 工程タブ・ボタン行など、画面全体の図で使う部品
WINDOW_PARTS = {
    "queue": uni(cls("NSTableView"), lbl("入力キュー", exact=True)),
    "queuebuttons": uni(btn("追加…"), btn("クリア")),
    "graph": uni(cls("QualityGraphView"), lbl("品質グラフ", exact=True)),
    "preview": PREVIEW_ONLY,
    "viewmode": uni(seglabel("フレーム"), seglabel("結果")),
    "frameslider": cls("NSSlider", section=""),
    "stepbuttons": uni(btn("←"), btn("→")),
    "zoom": uni(seglabel("全体"), seglabel("400%")),
    "display": ("find", dict(cls="NSPopUpButton", section="", text="", exact=True)),
    "apedit": btn("配置を編集"),
    "tabs": uni(seglabel("品質評価"), seglabel("仕上げ・出力")),
    "presets": uni(("find", dict(text="プリセット…", cls="NSPopUpButton")), btn("プリセットを保存…")),
    "inspector": rect(966, 64, 306, 676),
}


def process_buttons(first="品質評価"):
    return uni(btn(first), ("find", dict(text="書き出し", cls="NSButton", section="", nth=0)))


# ---- 図の定義 -------------------------------------------------------------------
#
# name: 出力名 / shot: 撮影名 / crop: 切り出し（spec か rect） / width: 出力の幅（画素）
# anchors: {名前: spec}

FIGURES = []


def fig(name, shot, crop=None, width=None, anchors=None, fmt="png", padding=0):
    FIGURES.append(dict(name=name, shot=shot, crop=crop, width=width, anchors=anchors or {}, fmt=fmt,
                        padding=padding))


# 第2章 画面の見方
fig("win_empty", "empty", width=1800, anchors={
    "queue": WINDOW_PARTS["queue"], "addbutton": WINDOW_PARTS["queuebuttons"], "graph": WINDOW_PARTS["graph"],
    "preview": WINDOW_PARTS["preview"], "tabs": WINDOW_PARTS["tabs"], "inspector": WINDOW_PARTS["inspector"],
    "process": uni(btn("品質評価"), btn("書き出し…")), "status": lbl("動画または静止画を追加してください"),
    "presets": WINDOW_PARTS["presets"],
})
fig("win_overview", "finished", width=1800, fmt="jpg", anchors={
    "queue": WINDOW_PARTS["queue"], "graph": WINDOW_PARTS["graph"], "preview": WINDOW_PARTS["preview"],
    "viewmode": WINDOW_PARTS["viewmode"], "frameslider": uni(cls("NSSlider", section=""), btn("→")),
    "zoom": WINDOW_PARTS["zoom"], "display": uni(WINDOW_PARTS["display"], btn("配置を編集")),
    "tabs": WINDOW_PARTS["tabs"], "inspector": WINDOW_PARTS["inspector"], "presets": WINDOW_PARTS["presets"],
    "process": uni(btn("品質を再評価"), btn("書き出し…")), "status": uni(("find", dict(text="RGBのずれ", cls="NSTextField"))),
    "banner": uni(lbl("枚を自動除外"), btn("×")),
})
fig("toolbar", "finished", crop=[232, 660, 736, 96], width=1472, fmt="jpg", anchors={
    "viewmode": WINDOW_PARTS["viewmode"], "order": uni(seglabel("時系列", 1), seglabel("品質順", 1)),
    "slider": cls("NSSlider", section=""), "step": uni(btn("←"), btn("→")),
    "info": lbl("スタック結果"), "zoom": WINDOW_PARTS["zoom"],
    "display": WINDOW_PARTS["display"], "apedit": btn("配置を編集"),
    "apcount": lbl("位置合わせ領域 "),
})

# 第3章 はじめての処理（木星）
fig("qs_added", "added", width=1800, fmt="jpg", anchors={
    "addbutton": btn("追加…"), "queuerow": cls("NSTableView"), "preview": PREVIEW_ONLY,
    "slider": uni(cls("NSSlider", section=""), btn("→")), "framelabel": lbl("#1 / 4617"),
    "quality": btn("品質評価"), "status": lbl("jupiter.ser —"), "metric": popup("quality", 0),
})
fig("qs_quality", "quality", width=1800, fmt="jpg", anchors={
    "graph": cls("QualityGraphView"), "cutline": ("cutline", cls("QualityGraphView")),
    "graphmode": uni(seglabel("時系列", 0), seglabel("品質順", 0)),
    "slider": uni(cls("NSSlider", section=""), btn("→")), "framelabel": lbl("上位 0.1%"),
    "align": btn("アライメント", section=""), "tabs": WINDOW_PARTS["tabs"],
    "refslider": ("find", dict(cls="NSSlider", section="align", nth=1)),
    "method": popup("align", 0),
})
fig("graph_order", "quality", crop=pad(union([[8, 236, 224, 504]]), 2), width=460, anchors={
    "cutline": ("cutline", cls("QualityGraphView")), "mode": uni(seglabel("時系列", 0), seglabel("品質順", 0)),
    "best": rect(10, 290, 30, 60), "worst": rect(196, 600, 36, 90),
})
fig("graph_timeline", "quality_timeline", crop=pad(union([[8, 236, 224, 504]]), 2), width=460, anchors={
    "cutline": ("cutline", cls("QualityGraphView")), "mode": uni(seglabel("時系列", 0), seglabel("品質順", 0)),
})
fig("frame_best", "quality", crop=[400, 190, 400, 400], width=600, fmt="jpg")
fig("frame_worst", "quality_worst", crop=[400, 190, 400, 400], width=600, fmt="jpg")
fig("qs_align", "align", width=1800, fmt="jpg", anchors={
    "preview": PREVIEW_ONLY, "banner": uni(lbl("枚を自動除外"), btn("×")), "details": btn("詳細…"),
    "viewmode": uni(seglabel("フレーム"), seglabel("結果")), "ref": seglabel("参照"),
    "apcount": lbl("位置合わせ領域 110"), "stack": btn("スタック", section=""),
    "tabs": WINDOW_PARTS["tabs"], "aptop": ("find", dict(cls="NSSlider", section="stack", nth=0)),
    "method": popup("stack", 0), "graph": cls("QualityGraphView"),
})
fig("ap_zoom", "align", crop=[330, 60, 540, 540], width=900, fmt="jpg")
fig("heatmap", "heatmap", crop=[238, 36, 724, 648], width=1000, fmt="jpg", anchors={
    "legend": rect(240, 645, 132, 28),
})
fig("qs_stacked", "stacked", width=1800, fmt="jpg", anchors={
    "preview": PREVIEW_ONLY, "result": seglabel("結果"), "tabs": WINDOW_PARTS["tabs"],
    "restack": btn("再スタック"), "export": btn("書き出し…"), "status": lbl("完了"),
    "wavelet": sec("wavelet"),
})
fig("before_stack", "stacked", crop=[410, 180, 380, 380], width=640, fmt="jpg")
fig("after_nowave", "finished_nowave", crop=[410, 180, 380, 380], width=640, fmt="jpg")
fig("after_finish", "finished", crop=[410, 180, 380, 380], width=640, fmt="jpg")
fig("qs_export", "export", crop=[962, 0, 318, 420], width=636, anchors={
    "format": popup("export", 0), "name": popup("export", 1), "object": ("find", dict(cls="NSTextField", section="export", text="", exact=True, nth=0)),
    "meta": btn("処理条件と撮影時刻をファイルに記録する"), "filename": lbl("jupiter_ap48"),
    "save": btn("名前を付けて書き出し…"), "multi": uni(("find", dict(cls="NSTextField", section="export", text="5, 10, 25")), btn("まとめて書き出し…")),
})

# 第4章 設定パネル（縦に全部つないだもの）
INSPECTOR_FIGS = [
    # (図名, 撮影, 設定欄の並び)
    ("insp_quality", "added_insp", ["quality"]),
    ("insp_roi", "added_insp", ["roi"]),
    ("insp_input", "added_insp", ["input"]),
    ("insp_qadv", "added_insp", ["qualityAdvanced"]),
    ("insp_align", "align_tab1_insp", ["align"]),
    ("insp_aadv", "align_tab1_insp", ["alignAdvanced"]),
    ("insp_stack", "stack_tab2_insp", ["stack"]),
    ("insp_drizzle", "stack_tab2_insp", ["drizzle"]),
    ("insp_compare", "finished_insp", ["compare"]),
    ("insp_channel", "finished_insp", ["channel"]),
    ("insp_wavelet", "finished_insp", ["wavelet"]),
    ("insp_color", "finished_insp", ["color"]),
    ("insp_tone", "finished_insp", ["tone"]),
    ("insp_geometry", "finished_insp", ["geometry"]),
    ("insp_export", "finished_insp", ["export"]),
]


def inspector_anchors(shot, keys):
    """設定欄の中の部品すべてに、文言から名前を付ける（例: quality/popup1、wavelet/Layer 1）。"""
    return {}


# 各設定欄の中で印を付ける部品（{名前: spec}）。spec は設定欄で絞って探す。
def s_find(section, text=None, c=None, nth=0, exact=False):
    return ("find", dict(section=section, text=text, cls=c, nth=nth, exact=exact))


INSPECTOR_ANCHORS = {
    "insp_quality": {
        "metric": s_find("quality", c="NSPopUpButton", nth=0),
        "byteorder": s_find("quality", c="NSPopUpButton", nth=1),
        "depth": s_find("quality", c="NSPopUpButton", nth=2),
    },
    "insp_roi": {
        "draw": s_find("roi", "フレームの上で処理範囲を描く", "NSButton"),
        "label": s_find("roi", "全体を処理します", "NSTextField"),
        "clear": s_find("roi", "全体に戻す", "NSButton"),
    },
    "insp_input": {
        "range": uni(s_find("input", c="NSTextField", text="", exact=True, nth=0),
                     s_find("input", c="NSTextField", text="", exact=True, nth=1)),
        "bayer": s_find("input", c="NSPopUpButton", nth=0),
        "debayer": s_find("input", c="NSPopUpButton", nth=1),
        "dark": s_find("input", "ダークを選ぶ…", "NSButton"),
        "flat": s_find("input", "フラットを選ぶ…", "NSButton"),
        "clearcal": s_find("input", "補正をやめる", "NSButton"),
    },
    "insp_qadv": {
        "outlier": s_find("qualityAdvanced", "6.0", "NSTextField", exact=True),
        "similarity": s_find("qualityAdvanced", "0.5", "NSTextField", exact=True),
        "maxshift": s_find("qualityAdvanced", "", "NSTextField", exact=True),
    },
    "insp_align": {
        "method": s_find("align", c="NSPopUpButton", nth=0),
        "mode": s_find("align", c="NSSegmentedControl", nth=0),
        "apsize": s_find("align", c="NSPopUpButton", nth=1),
        "search": s_find("align", c="NSSlider", nth=0),
        "placement": uni(s_find("align", "自動配置に戻す", "NSButton"), s_find("align", "すべて消去", "NSButton")),
        "ref": s_find("align", c="NSSlider", nth=1),
        "refine": s_find("align", "参照の反復精密化（2パス）", "NSButton"),
        "report": s_find("align", "対象モード:", "NSTextField"),
    },
    "insp_aadv": {
        "minscore": s_find("alignAdvanced", "0.5", "NSTextField", exact=True),
        "gradient": s_find("alignAdvanced", "0.6", "NSTextField", exact=True),
        "level": s_find("alignAdvanced", "0.15", "NSTextField", exact=True),
    },
    "insp_stack": {
        "selmode": s_find("stack", c="NSSegmentedControl", nth=0),
        "aptop": s_find("stack", c="NSSlider", nth=0),
        "normalize": s_find("stack", "輝度正規化", "NSButton"),
        "method": s_find("stack", c="NSPopUpButton", nth=0),
        "sigma": s_find("stack", "2.0", "NSTextField", exact=True),
        "lowmem": s_find("stack", "低メモリモード（2GB上限）", "NSButton"),
        "rawcfa": s_find("stack", "Bayerのまま合成する（デバイヤーしない）", "NSButton"),
    },
    "insp_drizzle": {
        "scale": s_find("drizzle", c="NSSegmentedControl", nth=0),
        "pixfrac": s_find("drizzle", c="NSSlider", nth=0),
        "diagnose": s_find("drizzle", "ドリズルを診断", "NSButton"),
        "result": s_find("drizzle", "効果は小さそう", "NSTextField"),
    },
    "insp_compare": {
        "all": s_find("compare", "仕上げ全体の効果をプレビュー", "NSButton"),
    },
    "insp_channel": {
        "fields": uni(s_find("channel", "R x", "NSTextField", exact=True), s_find("channel", c="NSTextField", nth=5)),
        "auto": s_find("channel", "自動で合わせる", "NSButton"),
        "reset": s_find("channel", "戻す", "NSButton", exact=True),
    },
    "insp_wavelet": {
        "preview": s_find("wavelet", "ウェーブレットの効果をプレビュー", "NSButton"),
        "layer1": uni(s_find("wavelet", "Layer 1", "NSTextField"), s_find("wavelet", c="NSSlider", nth=1),
                      s_find("wavelet", "初期値に戻す", "NSButton", nth=0)),
        "sharpen1": s_find("wavelet", c="NSSlider", nth=0),
        "denoise1": s_find("wavelet", c="NSSlider", nth=1),
        "reset1": s_find("wavelet", "初期値に戻す", "NSButton", nth=0),
        "linked": s_find("wavelet", "レイヤーを連動（配分を保って強さを変える）", "NSButton"),
        "dering": s_find("wavelet", c="NSSlider", nth=13),
        "resetall": s_find("wavelet", "すべてリセット", "NSButton"),
    },
    "insp_color": {
        "gains": uni(s_find("color", c="NSSlider", nth=0), s_find("color", c="NSSlider", nth=2)),
        "saturation": s_find("color", c="NSSlider", nth=3),
        "auto": s_find("color", "自動（灰色仮説）", "NSButton"),
    },
    "insp_tone": {
        "channel": s_find("tone", c="NSPopUpButton", nth=0),
        "histogram": s_find("tone", c="LevelsView", nth=0),
        "fields": uni(s_find("tone", c="NSTextField", text="0", exact=True), s_find("tone", c="NSTextField", text="255", exact=True)),
        "auto": s_find("tone", "自動", "NSButton", exact=True),
        "reset": s_find("tone", "初期値に戻す", "NSButton"),
    },
    "insp_geometry": {
        "rotate": uni(s_find("geometry", "↺ 左へ90°", "NSButton"), s_find("geometry", "右へ90° ↻", "NSButton")),
        "flip": uni(s_find("geometry", "左右反転", "NSButton"), s_find("geometry", "上下反転", "NSButton")),
        "cropmode": s_find("geometry", "プレビューで切り抜く枠を描く", "NSButton"),
        "cropbuttons": uni(s_find("geometry", "切り抜く", "NSButton", exact=True), s_find("geometry", "元に戻す", "NSButton")),
    },
    "insp_export": {
        "format": s_find("export", c="NSPopUpButton", nth=0),
        "name": s_find("export", c="NSPopUpButton", nth=1),
        "object": s_find("export", c="NSTextField", text="", exact=True, nth=0),
        "meta": s_find("export", "処理条件と撮影時刻をファイルに記録する", "NSButton"),
        "save": s_find("export", "名前を付けて書き出し…", "NSButton"),
        "multi": uni(s_find("export", c="NSTextField", text="5, 10, 25"), s_find("export", "まとめて書き出し…", "NSButton")),
    },
}

for name, shot, keys in INSPECTOR_FIGS:
    fig(name, shot, crop=("sections", keys), width=None, anchors=INSPECTOR_ANCHORS.get(name, {}), padding=0)

# 第5章 いろいろな素材
fig("moon_ap", "moon", crop=[238, 36, 724, 648], width=1100, fmt="jpg")
fig("sun_banner", "sun", crop=[232, 0, 736, 740], width=1100, fmt="jpg", anchors={
    "banner": uni(lbl("ヘッダと異なるバイトオーダー"), btn("×")), "buttons": uni(btn("little"), btn("big")),
})
fig("mp4_banner", "mp4", crop=[232, 0, 736, 60], width=1472, anchors={
    "banner": uni(lbl("MOV・MP4"), btn("×")),
})
fig("cr2_roi", "cr2_roi", width=1800, fmt="jpg", anchors={
    "queuerow": cls("NSTableView"), "roibox": ("imgrect", (6240, 4160, 1560, 1040, 3120, 2080)), "draw": btn("フレームの上で処理範囲を描く"),
    "label": lbl("処理範囲: "), "clear": btn("全体に戻す"), "status": lbl("処理範囲を変えました"),
})
fig("cropbox", "cropbox", crop=[232, 0, 1048, 740], width=1600, fmt="jpg", anchors={
    "box": ("imgrect", (448, 448, 64, 64, 320, 320)), "cropmode": btn("プレビューで切り抜く枠を描く"),
    "apply": btn("切り抜く", section="geometry"), "clearbox": btn("枠を消す"), "undo": btn("元に戻す", section="geometry"),
    "rotate": uni(btn("↺ 左へ90°"), btn("右へ90° ↻")),
})
fig("levels", "levels", crop=[962, 60, 318, 300], width=636, anchors={
    "channel": popup("tone", 0), "histogram": cls("LevelsView"),
    "black": ("levelmark", 12 / 255), "white": ("levelmark", 240 / 255),
    "mid": ("levelmark", (12 + 0.5 ** 1.15 * (240 - 12)) / 255),
    "fields": uni(("find", dict(cls="NSTextField", section="tone", text="12", exact=True)),
                  ("find", dict(cls="NSTextField", section="tone", text="240", exact=True))),
    "auto": btn("自動", section="tone"),
})
fig("drizzle_result", "stack_tab2", crop=[962, 60, 318, 420], width=636)


# ---- 生成 ----------------------------------------------------------------------

def crop_rect(shot, spec):
    if spec is None:
        return [0, 0, shot.size[0], shot.size[1]]
    if isinstance(spec, tuple) and spec[0] == "sections":
        r = union([shot.section_rect(k) for k in spec[1]])
        top = max(0, r[1] - 4)
        bottom = min(shot.size[1], r[1] + r[3] + 8)
        # 次の設定欄の見出しが写り込まないよう、その手前で切る
        for e in shot.items:
            if e["section"].startswith("header:") and e["rect"][1] > r[1] + r[3] - 1:
                bottom = min(bottom, e["rect"][1] - 3)
        return [0, top, shot.size[0], bottom - top]
    if isinstance(spec, tuple):
        return pad(resolve(shot, spec), 4)
    return list(spec)


def fmt_num(v):
    return f"{v:.4f}"


def build(only=None):
    os.makedirs(FIG, exist_ok=True)
    cache = {}
    for f in FIGURES:
        if only and f["name"] not in only:
            continue
        shot = cache.get(f["shot"]) or cache.setdefault(f["shot"], Shot(f["shot"]))
        cx, cy, cw, ch = crop_rect(shot, f["crop"])
        s = shot.scale
        box = (round(cx * s), round(cy * s), round((cx + cw) * s), round((cy + ch) * s))
        im = shot.image.crop(box)
        if f["width"] and im.size[0] > f["width"]:
            im = im.resize((f["width"], round(im.size[1] * f["width"] / im.size[0])), Image.LANCZOS)
        ext = f["fmt"]
        out = os.path.join(FIG, f"{f['name']}.{ext}")
        for old in (".png", ".jpg"):
            p = os.path.join(FIG, f["name"] + old)
            if os.path.exists(p) and p != out:
                os.remove(p)
        if ext == "jpg":
            im.save(out, quality=88, optimize=True, progressive=True)
        else:
            im = im.quantize(colors=256, method=Image.Quantize.MEDIANCUT) if f.get("quantize") else im
            im.save(out, optimize=True)
        lines = [f"% 自動生成（tools/figures.py）: {f['name']} ← {f['shot']}",
                 f"\\def\\lsaspect{{{fmt_num(ch / cw)}}}"]
        for aname, spec in f["anchors"].items():
            r = resolve(shot, spec)
            x0 = (r[0] - cx) / cw
            x1 = (r[0] + r[2] - cx) / cw
            y1 = 1 - (r[1] - cy) / ch
            y0 = 1 - (r[1] + r[3] - cy) / ch
            clamp = lambda v: max(0.0, min(1.0, v))
            lines.append(f"\\lsanchor{{{aname}}}{{{fmt_num(clamp(x0))}}}{{{fmt_num(clamp(y0))}}}"
                         f"{{{fmt_num(clamp(x1))}}}{{{fmt_num(clamp(y1))}}}")
        with open(os.path.join(FIG, f["name"] + ".coords.tex"), "w", encoding="utf-8") as fp:
            fp.write("\n".join(lines) + "\n")
        print(f"図: {f['name']:16s} {im.size[0]}×{im.size[1]}  {os.path.getsize(out) // 1024} KB")


def list_items(name, section=None):
    shot = Shot(name)
    for e in sorted(shot.items, key=lambda e: (e["rect"][1], e["rect"][0])):
        if section and e["section"] != section and e["section"] != "header:" + section:
            continue
        print(f"{e['class']:24s} {e['section']:22s} {[round(v) for v in e['rect']]}  {e['text'][:40]!r}")


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "--list":
        list_items(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
    else:
        build(sys.argv[1:] or None)
