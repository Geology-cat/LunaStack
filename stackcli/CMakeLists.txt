add_executable(stackcli src/main.cpp)
target_link_libraries(stackcli PRIVATE stackcore)
# 版はアプリと同じくプロジェクトの版から取る（出力画像の作成ソフトの記録にも入る）。
target_compile_definitions(stackcli PRIVATE LUNASTACK_VERSION="${PROJECT_VERSION}")
