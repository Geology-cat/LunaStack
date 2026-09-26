#pragma once

// 画像の行を塊に分けて並列に処理する（エンジン内部用）。
//
// **1画素あたりの演算の順序は変えない。** 行ごとに独立した計算（画素ごとの演算、
// 読み出し専用の入力からの畳み込みなど）だけを分けるので、結果はスレッド数や
// 実行順によらずバイト単位で一致する（仕様書 §7.1 の決定論性）。
// 行をまたいで足し込む処理（合計・最小最大など）はここに載せないこと。

#include <dispatch/dispatch.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>

namespace stackcore {
namespace detail {

// fn(y0, y1) を [y0, y1) の行について呼ぶ。小さな画像では分けずにそのまま呼ぶ。
template <class Fn>
void parallel_rows(int rows, const Fn& fn, int min_rows_per_task = 8) {
    if (rows <= 0) return;
    min_rows_per_task = std::max(1, min_rows_per_task);
    // 1塊を小さくしすぎると割り振りの手間が勝つ。コア数の数倍の塊に分ける。
    const int max_tasks = 64;
    const int tasks = std::max(1, std::min(max_tasks, rows / min_rows_per_task));
    if (tasks <= 1) {
        fn(0, rows);
        return;
    }

    struct Context {
        const Fn* function = nullptr;
        int rows = 0;
        int tasks = 0;
        std::atomic<bool> failed{false};
        std::mutex failure_mutex;
        std::exception_ptr failure;
    } context;
    context.function = &fn;
    context.rows = rows;
    context.tasks = tasks;

    dispatch_apply_f(
        static_cast<std::size_t>(tasks), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
        &context, [](void* raw, std::size_t i) {
            Context* c = static_cast<Context*>(raw);
            if (c->failed.load(std::memory_order_relaxed)) return;
            const long long lo = static_cast<long long>(c->rows) * static_cast<long long>(i) / c->tasks;
            const long long hi =
                static_cast<long long>(c->rows) * static_cast<long long>(i + 1) / c->tasks;
            try {
                (*c->function)(static_cast<int>(lo), static_cast<int>(hi));
            } catch (...) {
                std::lock_guard<std::mutex> lock(c->failure_mutex);
                if (!c->failure) c->failure = std::current_exception();
                c->failed.store(true, std::memory_order_relaxed);
            }
        });

    if (context.failure) std::rethrow_exception(context.failure);
}

}  // namespace detail
}  // namespace stackcore
