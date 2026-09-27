#!/bin/bash
# 説明書のスクリーンショットを撮る（アプリを場面ごとに起動し、画面と部品の位置を書き出す）。
#
# 使い方: docs/manual/tools/make_shots.sh [場面名 ...]   （省略するとすべて）
# 出力:   docs/manual/build/raw/<場面>.png / .json（ウインドウ）、<場面>_insp.png / .json（設定パネルの全体）
#
# 素材（sample-data/ は各自用意。README 参照）:
#   sample-data/jupiter.ser                               木星（カラー・448×448）
#   sample-data/2024-01-19-0224_6-U-L-Moon.ser            月（カラー・1024×768）
#   sample-data/2018-05-24-0250_6-JC-L-Sun_HAlpha_pipp.ser 太陽Hα（モノクロ16bit）
#   sample-data/manual/cr2/                               カメラのRAW（CR2 8枚）
#   sample-data/manual/jup.mp4                            木星（H.264）
# アプリは .build/local（なければ dist/）のものを使う。見出しの開閉は利用者の設定を書き換えず、
# 起動引数（-section.<key> YES）で上書きする。
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"
OUT="$ROOT/docs/manual/build/raw"
mkdir -p "$OUT"
APP="$ROOT/.build/local/LunaStackApp/LunaStack.app/Contents/MacOS/LunaStack"
[ -x "$APP" ] || APP="$ROOT/dist/LunaStack.app/Contents/MacOS/LunaStack"

JUP="$ROOT/sample-data/jupiter.ser"
MOON="$ROOT/sample-data/2024-01-19-0224_6-U-L-Moon.ser"
SUN="$ROOT/sample-data/2018-05-24-0250_6-JC-L-Sun_HAlpha_pipp.ser"
CR2="$ROOT/sample-data/manual/cr2"
MP4="$ROOT/sample-data/manual/jup.mp4"
# 作例の仕上げ（木星）
FIN="channel=1,wb=1,wavelet=12:7:3:1.5:1:1,denoise=0.45:0.25:0.05:0:0:0"

SECTIONS=(quality input roi qualityAdvanced align alignAdvanced stack drizzle compare channel wavelet color tone geometry export)

# shot 名前 [環境変数=値 ...]
shot() {
    local name=$1
    shift
    local args=()
    for k in "${SECTIONS[@]}"; do args+=(-section.$k YES); done
    # 外観はいつも明るい方（ダークモードの Mac で撮っても同じ見た目にする）
    args+=(-NSRequiresAquaSystemAppearance YES)
    SECONDS=0
    rm -f "$OUT/$name".png "$OUT/$name".json "$OUT/${name}_insp".png "$OUT/${name}_insp".png.json
    env LUNASTACK_SNAPSHOT="$OUT/$name.png" LUNASTACK_LAYOUT="$OUT/$name.json" LUNASTACK_TAB_NORMAL=1 \
        LUNASTACK_SIZE=1280x800 "$@" "$APP" "${args[@]}" >/dev/null 2>&1
    if [ -f "$OUT/$name.png" ]; then echo "撮影: $name（$SECONDS 秒）"; else echo "失敗: $name" >&2; fi
}

# 解析結果のキャッシュ（サイドカー）を消して、毎回同じ状態から撮る（sample-data の中だけ）。
fresh() {
    for f in "$@"; do rm -f "$f".lstk "$f".lstk.json "$f".lstkq "$f".lstkq.json; done
}

want() {
    [ $# -eq 0 ] && return 0
    return 1
}
SELECTED=("$@")
run() {
    local name=$1
    if [ ${#SELECTED[@]} -gt 0 ]; then
        local hit=0
        for s in "${SELECTED[@]}"; do [ "$s" = "$name" ] && hit=1; done
        [ $hit -eq 1 ] || return 0
    fi
    shot "$@"
}

J=(LUNASTACK_OPEN="$JUP")
fresh "$JUP" "$MOON" "$SUN" "$MP4"
rm -f "$CR2/cr2.lstk" "$CR2/cr2.lstk.json" "$CR2/cr2.lstkq" "$CR2/cr2.lstkq.json"

# 1. 起動直後・追加直後（解析前）
run empty LUNASTACK_TAB=0 LUNASTACK_INSPECTOR_SHOT="$OUT/empty_insp.png"
run added "${J[@]}" LUNASTACK_TAB=0 LUNASTACK_INSPECTOR_SHOT="$OUT/added_insp.png"
# 2. 品質評価のあと（最良・最悪のフレーム）
run quality "${J[@]}" LUNASTACK_AUTORUN=1 LUNASTACK_MODE=analyze LUNASTACK_INSPECTOR_SHOT="$OUT/quality_insp.png"
run quality_worst "${J[@]}" LUNASTACK_AUTORUN=1 LUNASTACK_MODE=analyze LUNASTACK_FRAMEPOS=999999
run quality_timeline "${J[@]}" LUNASTACK_AUTORUN=1 LUNASTACK_MODE=analyze LUNASTACK_GRAPH=0
# 3. アライメントのあと
run align "${J[@]}" LUNASTACK_AUTORUN=1 LUNASTACK_MODE=alignment
run align_tab1 "${J[@]}" LUNASTACK_AUTORUN=1 LUNASTACK_MODE=alignment LUNASTACK_TAB=1 LUNASTACK_INSPECTOR_SHOT="$OUT/align_tab1_insp.png"
# 除外（壊れたフレーム）を反映したあとのグラフと、採用されたうち最良・最悪のフレーム
run align_best "${J[@]}" LUNASTACK_AUTORUN=1 LUNASTACK_MODE=staged LUNASTACK_FRAMEPOS=0 LUNASTACK_TAB=0
run align_worst "${J[@]}" LUNASTACK_AUTORUN=1 LUNASTACK_MODE=staged LUNASTACK_FRAMEPOS=4586 LUNASTACK_TAB=0
run align_timeline "${J[@]}" LUNASTACK_AUTORUN=1 LUNASTACK_MODE=alignment LUNASTACK_GRAPH=0 LUNASTACK_TAB=0
run heatmap "${J[@]}" LUNASTACK_AUTORUN=1 LUNASTACK_MODE=alignment LUNASTACK_HEATMAP=1
# 4. スタックのあと
run stacked "${J[@]}" LUNASTACK_AUTORUN=1 LUNASTACK_MODE=staged
run stack_tab2 "${J[@]}" LUNASTACK_AUTORUN=1 LUNASTACK_MODE=staged LUNASTACK_TAB=2 LUNASTACK_DZDIAG=1 LUNASTACK_INSPECTOR_SHOT="$OUT/stack_tab2_insp.png"
# 5. 仕上げ
run finished "${J[@]}" LUNASTACK_AUTORUN=1 LUNASTACK_MODE=staged LUNASTACK_FINISH="$FIN" LUNASTACK_INSPECTOR_SHOT="$OUT/finished_insp.png"
run finished_nowave "${J[@]}" LUNASTACK_AUTORUN=1 LUNASTACK_MODE=staged LUNASTACK_FINISH="$FIN" LUNASTACK_WAVELET_OFF=1
run levels "${J[@]}" LUNASTACK_AUTORUN=1 LUNASTACK_MODE=staged LUNASTACK_FINISH="$FIN,levels=12:1.15:240" LUNASTACK_SECTION=tone
run cropbox "${J[@]}" LUNASTACK_AUTORUN=1 LUNASTACK_MODE=staged LUNASTACK_FINISH="$FIN" LUNASTACK_CROPBOX=64:64:320:320 LUNASTACK_SECTION=geometry
run export "${J[@]}" LUNASTACK_AUTORUN=1 LUNASTACK_MODE=staged LUNASTACK_FINISH="$FIN" LUNASTACK_SECTION=export
# 6. ほかの素材
run moon "LUNASTACK_OPEN=$MOON" LUNASTACK_LIMIT=300 LUNASTACK_AUTORUN=1 LUNASTACK_MODE=alignment
run sun "LUNASTACK_OPEN=$SUN" LUNASTACK_LIMIT=300 LUNASTACK_AUTORUN=1 LUNASTACK_MODE=staged
run cr2_roi "LUNASTACK_OPEN=$CR2" LUNASTACK_ROI=center LUNASTACK_TAB=0
run mp4 "LUNASTACK_OPEN=$MP4" LUNASTACK_TAB=0
