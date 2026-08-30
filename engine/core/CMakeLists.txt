# engine/core/CMakeLists.txt — Core Error / Result 系统（TASK-001）
#
# 目标 mmo::core_error 是 engine/core 模块对外的唯一接口别名，
# 下游只能通过 include/ 下的公开头调用。
#
# MMO_TRY 宏使用 GNU 语句表达式（({ ... })）实现 Rust 式 ? 提前返回，
# 因此需要对本子树开启 CXX_EXTENSIONS（根 CMakeLists 默认关闭）。
# 该扩展仅服务于错误传播宏，不参与任何运行时语义。

set(CMAKE_CXX_EXTENSIONS ON)

# ---- 库目标：纯值类型 Error + 错误码映射表 ----
add_library(mmo_core_error STATIC
    src/error/error_code.cpp
    src/error/error.cpp
)
add_library(mmo::core_error ALIAS mmo_core_error)

target_include_directories(mmo_core_error PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(mmo_core_error PUBLIC cxx_std_20)
mmorpg_add_warnings(mmo_core_error)

# ---- 库目标：TASK-002 Core Logger / Trace ----
# 与 mmo_core_error 相互独立：error 是纯值类型，log 依赖其 Result/Error 表达初始化失败，
# 依赖方向 log -> error，单向无环（禁止反向依赖）。
add_library(mmo_core_log STATIC
    src/log/log_level.cpp
    src/log/trace_id.cpp
    src/log/log_context.cpp
    src/log/log_formatter.cpp
    src/log/logger.cpp
    src/log/console_sink.cpp
    src/log/rotating_file_sink.cpp
)
add_library(mmo::core_log ALIAS mmo_core_log)

target_include_directories(mmo_core_log PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
# 内部头（环形队列 / 格式化器）只对本库与同模块白盒测试可见，不进 PUBLIC 接口。
target_include_directories(mmo_core_log PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(mmo_core_log PUBLIC cxx_std_20)
target_link_libraries(mmo_core_log PUBLIC mmo::core_error)
mmorpg_add_warnings(mmo_core_log)

# ---- 单元测试（自包含 harness，不依赖 gtest；vcpkg 离线不可用时仍可验证） ----
if(MMORPG_BUILD_TESTS)
    add_executable(core_error_test tests/error_test.cpp)
    target_link_libraries(core_error_test PRIVATE mmo::core_error)
    # MMO_TRY 宏的语句表达式扩展在 -Wpedantic 下会告警，关闭以保持输出干净（非错误）
    target_compile_options(core_error_test PRIVATE -Wno-pedantic)
    add_test(NAME Core_Error.Suite COMMAND core_error_test)
endif()

# ---- 单元测试：TASK-002 Core Logger / Trace ----
if(MMORPG_BUILD_TESTS)
    add_executable(core_log_test tests/log_test.cpp)
    target_link_libraries(core_log_test PRIVATE mmo::core_log)
    # 白盒访问本模块内部头（环形队列 / 行格式化器），不进 PUBLIC 接口
    target_include_directories(core_log_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(core_log_test PRIVATE -Wno-pedantic)
    add_test(NAME Core_Log.Suite COMMAND core_log_test)

    # §16 反向编译校验：非法格式串必须「编译期」报错，而非拖到运行期。
    # log_format_fail.cpp 故意写错格式串，这里断言它编译失败；若通过则判测试失败。
    add_test(NAME Core_Log.FormatStringNegative
        COMMAND ${CMAKE_COMMAND}
            -DCOMPILER=${CMAKE_CXX_COMPILER}
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/log_format_fail.cpp
            -DINCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/include
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/NegativeCompileCheck.cmake)
endif()

# ---- Benchmark（1e7 次，输出 bench/core_error.txt 的 key=value） ----
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(error_bench tests/error_bench.cpp)
    target_link_libraries(error_bench PRIVATE mmo::core_error)
    target_compile_options(error_bench PRIVATE -Wno-pedantic)

    # TASK-002：8 线程 × 10 万条压测，输出 bench/core_log.txt
    add_executable(log_bench tests/log_bench.cpp)
    target_link_libraries(log_bench PRIVATE mmo::core_log)
endif()
