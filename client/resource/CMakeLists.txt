# client/resource/CMakeLists.txt — TASK-036 Resource / Low Spec System
#
# 静态库 mmo_resource（alias mmo::resource）+ 测试 resource_test + 基准 resource_bench。
# 仅链接既有目标：mmo::client_core / mmo::protocol / mmo::core_error / mmo::core_time。
# 不触碰其它模块目录；所有源文件仅在本子树内。

add_library(mmo_resource STATIC
    src/quality_preset.cpp
    src/resource_manager.cpp
    src/scene_streamer.cpp)
add_library(mmo::resource ALIAS mmo_resource)

target_include_directories(mmo_resource PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(mmo_resource PUBLIC cxx_std_20)
target_link_libraries(mmo_resource
    PUBLIC  mmo::client_core
    PRIVATE mmo::core_error mmo::core_time)
mmorpg_add_warnings(mmo_resource)

# ---- 单元测试（ctest -R Resource）----
if(MMORPG_BUILD_TESTS)
    add_executable(resource_test tests/resource_test.cpp)
    target_link_libraries(resource_test PRIVATE
        mmo::resource
        mmo::client_core
        mmo::protocol
        mmo::core_error
        mmo::core_time)
    target_include_directories(resource_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(resource_test PRIVATE
        -Wno-pedantic -Wno-conversion -Wno-sign-conversion -Wno-float-conversion
        -Wno-unused-result)
    add_test(NAME Resource.Suite COMMAND resource_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（bin/resource_bench + bench/resource_low.txt）----
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(resource_bench benchmark/resource_bench.cpp)
    target_link_libraries(resource_bench PRIVATE
        mmo::resource
        mmo::client_core
        mmo::protocol
        mmo::core_error
        mmo::core_time)
    target_include_directories(resource_bench PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(resource_bench PRIVATE
        -Wno-pedantic -Wno-conversion -Wno-sign-conversion -Wno-float-conversion
        -Wno-unused-result)
    set_target_properties(resource_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
