#!/bin/sh
# GUIの自己検証を人手なしで通す（ctest の gui_selftest）。
#
# 合成SERを作り、アプリを LUNASTACK_SELFTEST 付きで起動して
# 品質評価 → アライメント → スタックの3工程を画面の経路で通す。
# 途中でスライダーの品質順・AP当たり判定・取り消し・プリセット・
# 画面と書き出しの一致を確かめ、1つでも失敗すれば終了コード1で終わる。
#
# GUIセッションの無い環境（SSHだけのCIなど）では起動できないので、
# LUNASTACK_SKIP_GUI_TEST を設定すると飛ばす（終了コード77＝スキップ）。
set -eu
APP="$1"
MAKER="$2"
if [ -n "${LUNASTACK_SKIP_GUI_TEST:-}" ]; then exit 77; fi
WORK="${TMPDIR:-/tmp}/lunastack_gui_selftest_$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
"$MAKER" "$WORK/scene.ser" 30
LUNASTACK_SNAPSHOT="$WORK/snapshot.png" \
LUNASTACK_OPEN="$WORK/scene.ser" \
LUNASTACK_AUTORUN=1 \
LUNASTACK_MODE=staged \
LUNASTACK_SELFTEST=1 \
LUNASTACK_DRIZZLE=2 \
LUNASTACK_FINISH="channel=auto,wb=auto,crop=auto,rotate=1,dering=0.5" \
LUNASTACK_SIZE=1000x640 \
"$APP" -AppleLanguages '(ja)'
test -s "$WORK/snapshot.png"
