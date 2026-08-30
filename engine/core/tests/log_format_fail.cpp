// engine/core/tests/log_format_fail.cpp — **故意无法通过编译**的源文件
//
// 用途：验证 §16「格式化非法格式串时编译期报错」。
// 该文件不参与常规构建，只由 `ctest -R Core_Log.FormatStringNegative` 反向校验：
// 编译它**必须失败**，若某天编译通过，说明格式串编译期校验失效，测试判失败。
//
// 手工复现命令（仓库根执行）：
//   g++ -std=c++20 -fsyntax-only -I engine/core/include engine/core/tests/log_format_fail.cpp
// 期望输出包含：`call to consteval function ... is not a constant expression`

#include "mmo/core/log/logger.h"

int main() {
    // 错误：格式串有 2 个占位符，却没有提供任何实参。
    MMO_LOG(::mmo::core::LogLevel::Info, "player={} skill={}");
    return 0;
}
