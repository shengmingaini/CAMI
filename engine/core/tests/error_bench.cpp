// engine/core/tests/error_bench.cpp — TASK-001 Benchmark
//
// 1e7 次 Result 构造+析构 -> result_ns_per_op
// 1e7 次失败路径传递     -> alloc_per_fail（operator new 计数 / 迭代次数）
// 输出机器可读 key=value 到 bench/core_error.txt（供验收脚本 assert_metric 解析）。

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/error/result.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <new>

namespace {
std::size_t g_alloc_count = 0;
}  // namespace

void* operator new(std::size_t n) {
    g_alloc_count++;
    void* p = std::malloc(n);
    if (!p) std::abort();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
    std::size_t iterations = 10000000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            iterations = static_cast<std::size_t>(std::strtoull(argv[i + 1], nullptr, 10));
        }
    }

    // ---- 1e7 次 Result<Ok> 构造+析构 ----
    // 用 volatile 接收结果 + 以迭代序号作值，强制编译器保留真实构造工作（避免被测循环被整体优化消除）。
    double result_ns_per_op = 0.0;
    {
        volatile int sink = 0;
        auto start = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < iterations; ++i) {
            mmo::core::Result<int> r =
                mmo::core::Result<int>::Ok(static_cast<int>(i & 0xFFFF));
            sink = r.Value();
        }
        auto end = std::chrono::steady_clock::now();
        double total_ns =
            std::chrono::duration<double, std::nano>(end - start).count();
        result_ns_per_op = total_ns / static_cast<double>(iterations);
        (void)sink;
    }

    // ---- 1e7 次失败路径传递（短 message，预期零堆分配） ----
    double alloc_per_fail = 0.0;
    {
        volatile int sink = 0;
        g_alloc_count = 0;
        for (std::size_t i = 0; i < iterations; ++i) {
            mmo::core::Result<int> r = mmo::core::Result<int>::Fail(
                mmo::core::Error(mmo::core::ErrorCode::BUSY, "fail"));
            auto r2 = std::move(r).AndThen(
                [](int) { return mmo::core::Result<int>::Ok(1); });
            sink = r2.HasValue() ? 1 : 0;
        }
        alloc_per_fail = static_cast<double>(g_alloc_count) /
                         static_cast<double>(iterations);
        (void)sink;
    }

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "result_ns_per_op=%.3f\nalloc_per_fail=%.0f\n",
                  result_ns_per_op, alloc_per_fail);
    std::printf("%s", buf);

    // 写 bench 输出文件（CWD = 仓库根，由验收脚本 mkdir -p bench 保证目录存在）
    FILE* f = std::fopen("bench/core_error.txt", "w");
    if (f) {
        std::fputs(buf, f);
        std::fclose(f);
    } else {
        std::fprintf(stderr, "WARN: cannot write bench/core_error.txt\n");
    }
    return 0;
}
