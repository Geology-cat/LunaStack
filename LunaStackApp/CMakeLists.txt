# AppKitアプリ（Objective-C++）。
#
# エンジン（libstackcore）をそのまま呼ぶ。Obj-C++はC++を直接コンパイルできるので
# FFI層は要らない（実装計画書 §2）。
#
# SwiftUIは10.15以降でしか使えないため、UIはAppKitを手書きする。
# xibは使わずコードで組み立てる。10.13から最新まで同じコードで動かすには、
# nibのバージョン差を持ち込まないほうが確実なため。

enable_language(OBJCXX)

set(LUNASTACK_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/Resources/LunaStack.icns")
set_source_files_properties(${LUNASTACK_APP_ICON} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources"
)

set(LUNASTACK_JA_LOCALIZATIONS
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/ja.lproj/Localizable.strings"
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/ja.lproj/InfoPlist.strings"
)
set(LUNASTACK_EN_LOCALIZATIONS
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/en.lproj/Localizable.strings"
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/en.lproj/InfoPlist.strings"
)
set_source_files_properties(${LUNASTACK_JA_LOCALIZATIONS} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources/ja.lproj"
)
set_source_files_properties(${LUNASTACK_EN_LOCALIZATIONS} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources/en.lproj"
)

# 同梱するライブラリのライセンス表示（LibRaw は CDDL 1.0 で使う。third_party/LibRaw）。
set(LUNASTACK_THIRD_PARTY_NOTICES
    "${PROJECT_SOURCE_DIR}/third_party/LibRaw/LICENSE.CDDL"
    "${PROJECT_SOURCE_DIR}/third_party/LibRaw/COPYRIGHT"
    "${PROJECT_SOURCE_DIR}/third_party/LibRaw/README.LunaStack.md"
)
set_source_files_properties(${LUNASTACK_THIRD_PARTY_NOTICES} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources/ThirdParty/LibRaw"
)

add_executable(LunaStackApp MACOSX_BUNDLE
    src/main.mm
    src/AppDelegate.mm
    src/LSAppSupport.mm
    src/MainWindowController.mm
    src/MainWindowController+Layout.mm
    src/MainWindowController+Queue.mm
    src/MainWindowController+Settings.mm
    src/MainWindowController+Jobs.mm
    src/MainWindowController+Preview.mm
    src/MainWindowController+Finishing.mm
    src/MainWindowController+SelfCheck.mm
    src/PreviewView.mm
    src/LevelsView.mm
    src/QualityGraphView.mm
    src/QueueItem.mm
    src/Presets.mm
    ${LUNASTACK_APP_ICON}
    ${LUNASTACK_JA_LOCALIZATIONS}
    ${LUNASTACK_EN_LOCALIZATIONS}
    ${LUNASTACK_THIRD_PARTY_NOTICES}
)

target_include_directories(LunaStackApp PRIVATE src)
target_link_libraries(LunaStackApp PRIVATE stackcore lunastack_flags)
target_link_libraries(LunaStackApp PRIVATE
    "-framework Cocoa"
    "-framework CoreGraphics"
)

# Obj-C++ にもC++と同じ規約を適用する。
set_target_properties(LunaStackApp PROPERTIES
    # バンドル名と実行ファイル名を揃える。
    # 揃っていないと Info.plist の CFBundleExecutable と実体が食い違い、
    # `open` で起動したときに何も言わず終了する（直接実行すると動くので気づきにくい）。
    OUTPUT_NAME "LunaStack"
    OBJCXX_STANDARD 17
    OBJCXX_STANDARD_REQUIRED ON
    MACOSX_BUNDLE_BUNDLE_NAME "LunaStack"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.lunastack.app"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
)

# ARCは使わない。エンジンとの境界でC++オブジェクトの寿命を明示的に扱うため、
# Obj-C側も手動管理に揃えておくほうが取り違えが起きにくい。
target_compile_options(LunaStackApp PRIVATE -fno-objc-arc)
