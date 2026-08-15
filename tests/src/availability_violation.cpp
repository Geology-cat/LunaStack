// このファイルは「ビルドが失敗すること」が期待される値である。
//
// -Werror=unguarded-availability が有効なら、デプロイメントターゲット(10.13)より
// 新しいAPIの使用はコンパイルエラーになる。もしこれがビルドできてしまったら、
// ガードが効いていない＝10.13対応が無検証である、ということを意味する。
//
// CTest 側で WILL_FAIL TRUE を指定して「失敗すること」を検証している
// （tests/CMakeLists.txt の availability_guard_fires）。

#include <filesystem>  // macOS 10.15 以降

int main() {
    std::filesystem::path p("/tmp");
    return std::filesystem::exists(p) ? 0 : 1;
}
