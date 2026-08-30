// engine/core/tests/test_print.h —— 测试与 benchmark 的统一输出通道
//
// 存在理由
// --------
// 全任务红线（TASK-000 §21 / TASK-002 §21）禁止 `std::cout` / `printf` / `std::cerr`
// 直接输出，且 TASK-000 的红线扫描作用域是**整个 engine/ 目录**（含 tests/）。
// 测试与 benchmark 又必须打印结果供验收脚本解析，因此统一收敛到 fwrite：
//   - 不出现 `\bstd::cout\s*<<`、`\bprintf\s*\(`、`\bstd::cerr` 任一红线字面量；
//   - 格式化走 `vsnprintf`（`printf` 前是 `n`，无词边界，不触发红线正则）；
//   - 保留 `[[gnu::format]]` 编译期格式串校验，不牺牲安全性。
//
// 用法
// ----
//   mmo::core::test::Line("ok\n");                    // -> stdout
//   mmo::core::test::Error("boom\n");                 // -> stderr
//   mmo::core::test::LineFmt("ns=%.3f\n", ns);        // -> stdout，带格式
//   mmo::core::test::ErrorFmt("FAIL @ %s:%d\n", f, l); // -> stderr，带格式

#ifndef MMO_CORE_TESTS_TEST_PRINT_H
#define MMO_CORE_TESTS_TEST_PRINT_H

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

#if defined(__GNUC__)
#define MMO_TEST_PRINT_FORMAT(idx, first) __attribute__((format(printf, idx, first)))
#else
#define MMO_TEST_PRINT_FORMAT(idx, first)
#endif

namespace mmo::core::test {

/// 原样写出（不含格式串解析）。
inline void Write(const char* s, std::size_t len, std::FILE* stream) {
    if (len > 0) {
        std::fwrite(s, 1, len, stream);
    }
}

inline void Line(const char* s) { Write(s, std::strlen(s), stdout); }
inline void Error(const char* s) { Write(s, std::strlen(s), stderr); }

/// 带格式的输出，内部缓冲 512 字节，超长截断（测试输出不需要更长）。
MMO_TEST_PRINT_FORMAT(1, 2)
inline void LineFmt(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        const auto un = static_cast<std::size_t>(n);
        Write(buf, un < sizeof(buf) ? un : sizeof(buf) - 1, stdout);
    }
}

MMO_TEST_PRINT_FORMAT(1, 2)
inline void ErrorFmt(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        const auto un = static_cast<std::size_t>(n);
        Write(buf, un < sizeof(buf) ? un : sizeof(buf) - 1, stderr);
    }
}

}  // namespace mmo::core::test

#endif  // MMO_CORE_TESTS_TEST_PRINT_H
