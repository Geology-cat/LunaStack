#!/bin/bash

# 配布用の DMG を dist/LunaStack-<版>.dmg に作る。
#
# 中身: LunaStack.app / LunaStack 使い方ガイド.pdf / かんたんインストーラ.scpt / Applications（別名）
# 先に ./scripts/build_app.sh で dist/LunaStack.app を作っておくこと。
# macOS 10.13 でも開けるよう、ファイルシステムは HFS+ にする。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DIST_DIR="${PROJECT_DIR}/dist"
APP="${DIST_DIR}/LunaStack.app"
MANUAL="${PROJECT_DIR}/docs/manual/LunaStack_使い方ガイド.pdf"
INSTALLER_SRC="${SCRIPT_DIR}/dmg/かんたんインストーラ.applescript"

if [ ! -d "${APP}" ]; then
    echo "dist/LunaStack.app がありません。先に ./scripts/build_app.sh を実行してください" >&2
    exit 1
fi
VERSION="$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' "${APP}/Contents/Info.plist")"
DMG="${DIST_DIR}/LunaStack-${VERSION}.dmg"
VOLUME="LunaStack ${VERSION}"

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/lunastack_dmg.XXXXXX")"
trap 'rm -rf "${STAGE}"' EXIT

ditto "${APP}" "${STAGE}/LunaStack.app"
cp "${MANUAL}" "${STAGE}/LunaStack 使い方ガイド.pdf"
osacompile -o "${STAGE}/かんたんインストーラ.scpt" "${INSTALLER_SRC}"
ln -s /Applications "${STAGE}/Applications"

# 署名が壊れていないこと（ditto でコピーしても保たれる）を確かめてから詰める。
codesign --verify --deep --strict "${STAGE}/LunaStack.app"

rm -f "${DMG}"
hdiutil create -volname "${VOLUME}" -srcfolder "${STAGE}" -fs HFS+ -format UDZO \
    -imagekey zlib-level=9 -ov "${DMG}" >/dev/null
hdiutil verify "${DMG}" >/dev/null

echo "完了: ${DMG}"
echo "大きさ: $(du -h "${DMG}" | cut -f1)"
