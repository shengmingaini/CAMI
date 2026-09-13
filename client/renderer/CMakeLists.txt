# client/renderer/CMakeLists.txt — TASK-035 Renderer（Headless 参考后端）
#
# 职责：
#   - mmo_renderer：Camera / MaterialSystem / Renderer（视锥剔除 / LOD / 合批 /
#     字节记账），纯 CPU 侧真实算法，不依赖 GPU。
#   - renderer_test：ctest 'Renderer.Suite'（§17 集成 + §20 验收 8 条核心）。
#   - render_bench：bin/render_bench，输出 bench/render_low.txt（§18）。
#
# 依赖：client_core（ClientWorld 插值源）/ core_error（Result/Error）。

add_library(mmo_renderer STATIC
    src/renderer.cpp)
add_library(mmo::renderer ALIAS mmo_renderer)

target_include_directories(mmo_renderer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(mmo_renderer PUBLIC cxx_std_20)
target_link_libraries(mmo_renderer PUBLIC
    mmo::client_core
    mmo::core_error)
mmorpg_add_warnings(mmo_renderer)

# ---- 单元测试（§17 集成 / §20 验收）----
if(MMORPG_BUILD_TESTS)
    add_executable(renderer_test tests/renderer_test.cpp)
    target_link_libraries(renderer_test PRIVATE
        mmo::renderer
        mmo::client_core
        mmo::protocol
        mmo::core_error
        mmo::core_time)
    target_include_directories(renderer_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(renderer_test PRIVATE -Wno-pedantic)
    add_test(NAME Renderer.Suite COMMAND renderer_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18 render_low.txt）----
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(render_bench benchmark/render_bench.cpp)
    target_link_libraries(render_bench PRIVATE
        mmo::renderer
        mmo::client_core
        mmo::protocol
        mmo::core_error
        mmo::core_time)
    target_include_directories(render_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(render_bench PRIVATE -Wno-pedantic)
    set_target_properties(render_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
