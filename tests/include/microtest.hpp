#pragma once

// 最小のテストハーネス。
// 外部依存を持たない理由:
//   * ネットワーク取得なしで旧OS上でもそのままビルド・実行できる
//   * CMake 4.x は cmake_minimum_required が 3.5 未満のプロジェクトを拒否するため、
//     FetchContent で古い依存を取り込むと壊れる
// （実装計画書 §2）

#include <cmath>
#include <cstdio>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

namespace microtest {

struct Failure {
    std::string message;
};

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back(TestCase{name, fn}); }
};

inline void fail(const std::string& message) { throw Failure{message}; }

template <typename T>
std::string mt_str(const T& value) {
    std::ostringstream os;
    os << value;
    return os.str();
}

inline int run_all() {
    int passed = 0;
    std::vector<std::string> failed;

    for (std::size_t i = 0; i < registry().size(); ++i) {
        const TestCase& c = registry()[i];
        try {
            c.fn();
            std::printf("  [  OK  ] %s\n", c.name);
            ++passed;
        } catch (const Failure& f) {
            std::printf("  [ FAIL ] %s\n           %s\n", c.name, f.message.c_str());
            failed.push_back(c.name);
        } catch (const std::exception& e) {
            std::printf("  [ FAIL ] %s\n           予期しない例外: %s\n", c.name, e.what());
            failed.push_back(c.name);
        } catch (...) {
            std::printf("  [ FAIL ] %s\n           不明な例外\n", c.name);
            failed.push_back(c.name);
        }
    }

    std::printf("\n%d / %d 件成功\n", passed, static_cast<int>(registry().size()));
    if (!failed.empty()) {
        std::printf("失敗したテスト:\n");
        for (std::size_t i = 0; i < failed.size(); ++i) {
            std::printf("  - %s\n", failed[i].c_str());
        }
        return 1;
    }
    return 0;
}

}  // namespace microtest

#define MT_TEST(name)                                                \
    static void name();                                              \
    static ::microtest::Registrar mt_registrar_##name(#name, &name); \
    static void name()

#define MT_FAIL_AT(msg)                                                                 \
    ::microtest::fail(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " + msg)

#define MT_CHECK(cond)                                       \
    do {                                                     \
        if (!(cond)) MT_FAIL_AT(std::string("条件が偽: " #cond)); \
    } while (0)

#define MT_CHECK_EQ(a, b)                                                              \
    do {                                                                               \
        auto mt_lhs = (a);                                                             \
        auto mt_rhs = (b);                                                             \
        if (!(mt_lhs == mt_rhs)) {                                                     \
            MT_FAIL_AT(std::string(#a " == " #b " が成立しません (左=") +               \
                       ::microtest::mt_str(mt_lhs) + ", 右=" +                          \
                       ::microtest::mt_str(mt_rhs) + ")");                             \
        }                                                                              \
    } while (0)

#define MT_CHECK_NEAR(a, b, tol)                                                       \
    do {                                                                               \
        const double mt_lhs = static_cast<double>(a);                                  \
        const double mt_rhs = static_cast<double>(b);                                  \
        if (std::fabs(mt_lhs - mt_rhs) > static_cast<double>(tol)) {                   \
            MT_FAIL_AT(std::string(#a " ≈ " #b " が成立しません (左=") +                \
                       ::microtest::mt_str(mt_lhs) + ", 右=" +                          \
                       ::microtest::mt_str(mt_rhs) + ", 許容差=" +                      \
                       ::microtest::mt_str(static_cast<double>(tol)) + ")");           \
        }                                                                              \
    } while (0)

#define MT_CHECK_THROWS(expr)                                              \
    do {                                                                   \
        bool mt_thrown = false;                                            \
        try {                                                              \
            expr;                                                          \
        } catch (...) {                                                    \
            mt_thrown = true;                                              \
        }                                                                  \
        if (!mt_thrown) MT_FAIL_AT(std::string("例外が投げられませんでした: " #expr)); \
    } while (0)
