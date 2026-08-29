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

# ---- 单元测试（自包含 harness，不依赖 gtest；vcpkg 离线不可用时仍可验证） ----
if(MMORPG_BUILD_TESTS)
    add_executable(core_error_test tests/error_test.cpp)
    target_link_libraries(core_error_test PRIVATE mmo::core_error)
    # MMO_TRY 宏的语句表达式扩展在 -Wpedantic 下会告警，关闭以保持输出干净（非错误）
    target_compile_options(core_error_test PRIVATE -Wno-pedantic)
    add_test(NAME Core_Error.Suite COMMAND core_error_test)
endif()

# ---- Benchmark（1e7 次，输出 bench/core_error.txt 的 key=value） ----
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(error_bench tests/error_bench.cpp)
    target_link_libraries(error_bench PRIVATE mmo::core_error)
    target_compile_options(error_bench PRIVATE -Wno-pedantic)
endif()
