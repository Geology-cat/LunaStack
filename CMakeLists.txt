cmake_minimum_required(VERSION 3.20)

# ---- 対応環境 ----------------------------------------------------------
# macOS 10.13 (High Sierra) 以降。仕様書 §2.1 参照。
#
# これらは project() より前に設定しなければならない。project() の時点で
# コンパイラ判定とターゲットトリプルの決定が行われるため、後から設定すると
# 黙ってSDKの既定値（＝ホストのmacOSバージョン）が使われてしまう。
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "最低対応macOSバージョン")

# Universal 2 は -DLUNASTACK_UNIVERSAL=ON で有効化する
# （開発機がIntelのためarm64は実行できず、ビルド・リンク検証のみ）。
option(LUNASTACK_UNIVERSAL "x86_64 + arm64 の Universal 2 としてビルドする" OFF)
if(LUNASTACK_UNIVERSAL)
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "" FORCE)
endif()

project(LunaStack VERSION 0.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# CMakeのReleaseは既定で -O3 を付けるが、最適化レベルは -O2 固定と決めている
# （仕様書 §7.1）。-O フラグが二重に並ぶ状態を避けるため既定値から外し、
# 最適化指定は lunastack_flags 一箇所に集約する。
set(CMAKE_C_FLAGS_RELEASE "-DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "-DNDEBUG" CACHE STRING "" FORCE)

# ---- 共通コンパイルフラグ ----------------------------------------------
# 決定論性の要件（仕様書 §7.1）のため -O2 固定。-ffast-math 系は禁止。
# 浮動小数点の結合則を崩すとスタック加算順序の固定が無意味になる。
add_library(lunastack_flags INTERFACE)
target_compile_options(lunastack_flags INTERFACE
    -O2
    -fno-fast-math
    -Wall
    -Wextra
    # デプロイメントターゲットより新しいAPIの使用をコンパイルエラーにする。
    # libc++ の std::filesystem 等にも availability 注釈が付いているため、
    # 手書きの禁止リストではなくコンパイラに機械的に弾かせる。
    # このガードが実際に発火することは tests/availability_guard で検証する。
    -Werror=unguarded-availability
    -Werror=unguarded-availability-new
)

add_subdirectory(third_party/LibRaw)
add_subdirectory(libstackcore)
add_subdirectory(stackcli)
add_subdirectory(LunaStackApp)

enable_testing()
add_subdirectory(tests)
