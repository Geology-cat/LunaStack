#!/bin/bash

# LunaStack の検証済みUniversalアプリを、常に dist/LunaStack.app へ作る。
# 中間生成物は .build/universal に集約し、リポジトリ直下にアプリを散在させない。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/.build/universal"
DIST_DIR="${PROJECT_DIR}/dist"
SOURCE_APP="${BUILD_DIR}/LunaStackApp/LunaStack.app"
DIST_APP="${DIST_DIR}/LunaStack.app"
PARALLEL_JOBS="$(sysctl -n hw.ncpu 2>/dev/null || printf '4')"

cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DLUNASTACK_UNIVERSAL=ON
cmake --build "${BUILD_DIR}" --parallel "${PARALLEL_JOBS}"
ctest --test-dir "${BUILD_DIR}" --output-on-failure

mkdir -p "${DIST_DIR}"
if [ -e "${DIST_APP}" ]; then
    rm -rf "${DIST_APP}"
fi
ditto "${SOURCE_APP}" "${DIST_APP}"

# 配布用証明書が無い環境でもバンドル全体の整合性を固定する。
codesign --force --deep --sign - "${DIST_APP}"
codesign --verify --deep --strict --verbose=2 "${DIST_APP}"

ARCHITECTURES="$(lipo -archs "${DIST_APP}/Contents/MacOS/LunaStack")"
case " ${ARCHITECTURES} " in
    *" x86_64 "*) ;;
    *) echo "エラー: x86_64 スライスがありません" >&2; exit 1 ;;
esac
case " ${ARCHITECTURES} " in
    *" arm64 "*) ;;
    *) echo "エラー: arm64 スライスがありません" >&2; exit 1 ;;
esac

VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
    "${DIST_APP}/Contents/Info.plist")"
echo "完了: ${DIST_APP}"
echo "バージョン: ${VERSION}"
echo "アーキテクチャ: ${ARCHITECTURES}"
