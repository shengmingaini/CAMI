# benchmark/common/CMakeLists.txt — TASK-025 公共基准设施
#
# 依赖方向（§27.3）：benchmark/common -> engine/core（error）
# 红线：分位数统计禁止全量排序（§15.1，固定桶直方图）。

add_library(mmo_bench_common STATIC
    src/metrics.cpp
    src/scenario.cpp
    src/system_probe.cpp)
add_library(mmo::bench_common ALIAS mmo_bench_common)

target_include_directories(mmo_bench_common PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_bench_common PUBLIC cxx_std_20)
target_link_libraries(mmo_bench_common PUBLIC
    mmo::core_error)
if(WIN32)
    # GetProcessMemoryInfo（RSS 探针），MinGW 由 libpsapi 提供
    target_link_libraries(mmo_bench_common PUBLIC psapi)
endif()
mmorpg_add_warnings(mmo_bench_common)
