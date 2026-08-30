# engine/core/cmake/NegativeCompileCheck.cmake
#
# 反向编译检查：给定源文件**必须编译失败**。
# 用于验证「本该在编译期被拒绝的代码真的被拒绝了」（如非法格式串）。
# 若源文件竟能编译通过，说明对应的编译期校验失效，测试判失败。
#
# 入参：-DCOMPILER=<c++ 编译器> -DSOURCE=<源文件> -DINCLUDE_DIR=<头文件目录>

if(NOT DEFINED COMPILER OR NOT DEFINED SOURCE OR NOT DEFINED INCLUDE_DIR)
    message(FATAL_ERROR "NegativeCompileCheck: 缺少 COMPILER / SOURCE / INCLUDE_DIR")
endif()

set(FLAGS "-std=c++20")
if(DEFINED EXTRA_FLAGS)
    separate_arguments(EXTRA_LIST NATIVE_COMMAND "${EXTRA_FLAGS}")
    list(APPEND FLAGS ${EXTRA_LIST})
endif()

execute_process(
    COMMAND ${COMPILER} ${FLAGS} -I${INCLUDE_DIR} -fsyntax-only ${SOURCE}
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)

if(rc EQUAL 0)
    message(FATAL_ERROR
        "反向编译检查失败：${SOURCE} 竟然编译通过\n"
        "期望：该源文件应因编译期校验（如格式串不匹配）而报错\n${out}")
endif()

message(STATUS "反向编译检查通过（符合预期，编译失败）：${SOURCE}")
