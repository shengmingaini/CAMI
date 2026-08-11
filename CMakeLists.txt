cmake_minimum_required(VERSION 3.20)
project(CAMI VERSION 0.1.0 LANGUAGES CXX)

# === C++ Standard ===
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# === Build Options ===
option(CAMI_BUILD_TESTS "Build unit tests" OFF)
option(CAMI_BUILD_BENCHMARK "Build benchmarks" ON)
option(CAMI_BUILD_MODULES "Build game modules (requires full deps)" OFF)
option(CAMI_USE_VCPKG "Use vcpkg for dependencies" OFF)

# === Output Directories ===
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# === Compiler Warnings ===
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter -Wthread-safety)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(/W4 /permissive- /Wno-unused-parameter)
endif()

# === vcpkg Integration ===
if(CAMI_USE_VCPKG AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain file")
    else()
        message(WARNING "VCPKG_ROOT not set. Please set it or pass -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake")
    endif()
endif()

# === Find Packages ===
# Boost 1.70+ 自带 CMake config 包。CMP0167=NEW 让 find_package 走 CONFIG 模式，
# 避免 CMake 3.30+ 对已移除的 FindBoost 模块发出策略警告（本地 Boost 1.91 / CI libboost-dev 均提供 config）。
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
find_package(Boost REQUIRED CONFIG COMPONENTS system)

# Heavy dependencies are only needed when building game modules (gRPC/Protobuf/etc).
# For the tech-stack verification (benchmark only), these are skipped.
if(CAMI_BUILD_MODULES)
    find_package(fmt CONFIG REQUIRED)
    find_package(spdlog CONFIG REQUIRED)
    find_package(gRPC CONFIG REQUIRED)
    find_package(Protobuf CONFIG REQUIRED)
    find_package(flatbuffers CONFIG REQUIRED)
    find_package(redis-plus-plus CONFIG REQUIRED)
    find_package(sol2 CONFIG REQUIRED)
    find_package(Lua REQUIRED)
endif()

# === Include Directories ===
include_directories(${CMAKE_SOURCE_DIR})

# === Subdirectories — 五层架构 (spec §3.1 / §3.2 单向依赖) ===
# 依赖方向: gateway → game → data → common ; ops → common (控制面)
add_subdirectory(common)      # 全局公共服务层
add_subdirectory(data)        # 数据管理层 (link common)
add_subdirectory(game)        # 逻辑业务层 (link common + data)
add_subdirectory(gateway)     # 接入层 (link common)
add_subdirectory(ops)         # 运维控制层 (link common)

# === Benchmark (tech stack verification) ===
if(CAMI_BUILD_BENCHMARK)
    add_subdirectory(benchmark)
endif()

# === Layered skeleton self-check (CI green proof) ===
# 零外部依赖：编译期验证五层已接入 monorepo 构建，且 目录↔分层↔target 一一对应。
enable_testing()
add_executable(cami_skeleton_check tests/unit/skeleton_layer_check.cpp)
target_link_libraries(cami_skeleton_check PRIVATE cami_gateway cami_game cami_data cami_common cami_ops)
add_test(NAME skeleton_layer_check COMMAND cami_skeleton_check)

# === Tests (GTest-based unit/integration, 重型依赖就绪后开启) ===
if(CAMI_BUILD_TESTS)
    find_package(GTest REQUIRED)
    # 各层实现后接入 add_subdirectory(tests)；此处直接注册已就绪的 codec 单测。
    add_executable(cami_codec_test tests/unit/codec_framing_test.cpp)
    target_link_libraries(cami_codec_test PRIVATE cami_gateway_codec GTest::gtest GTest::gtest_main)
    add_test(NAME codec_framing_test COMMAND cami_codec_test)

    # 周三交付物：心跳管理单测（保活/超时踢线/误差<1s/边界/无泄漏/空闲回收）。
    add_executable(cami_heartbeat_test tests/unit/heartbeat_test.cpp)
    target_link_libraries(cami_heartbeat_test PRIVATE cami_gateway_heartbeat GTest::gtest GTest::gtest_main)
    add_test(NAME heartbeat_test COMMAND cami_heartbeat_test)

    # 周四"单测补齐"：connection 模块非 socket 核心路径（FSM 全迁移 + io_context 池）。
    # cami_gateway_connection 链 Boost::system（Asio），故测试显式补链。
    add_executable(cami_connection_test tests/unit/connection_test.cpp)
    target_link_libraries(cami_connection_test PRIVATE cami_gateway_connection Boost::system GTest::gtest GTest::gtest_main)
    add_test(NAME connection_test COMMAND cami_connection_test)

    # Week3 周一：security 模块（token 鉴权 + AES-GCM；AES-GCM 真实路径在 MODULES=ON 下）。
    add_executable(cami_security_test tests/unit/security_test.cpp)
    target_link_libraries(cami_security_test PRIVATE cami_gateway_security GTest::gtest GTest::gtest_main)
    add_test(NAME security_test COMMAND cami_security_test)

    # Week3 周二：ratelimit 模块（令牌桶 + 连接频率 + 黑白名单；确定性假时钟）。
    add_executable(cami_ratelimit_test tests/unit/ratelimit_test.cpp)
    target_link_libraries(cami_ratelimit_test PRIVATE cami_gateway_ratelimit GTest::gtest GTest::gtest_main)
    add_test(NAME ratelimit_test COMMAND cami_ratelimit_test)

    # Week3 周三：router 模块（一致性哈希 + 热更新；迁移 <10%、均衡、确定性）。
    add_executable(cami_router_test tests/unit/router_test.cpp)
    target_link_libraries(cami_router_test PRIVATE cami_gateway_router GTest::gtest GTest::gtest_main)
    add_test(NAME router_test COMMAND cami_router_test)

    # Week3 周四：redis 模块（内存后端语义 + 16 分片均衡 + 故障切换；确定性 selfcheck）。
    add_executable(cami_redis_test tests/unit/redis_test.cpp)
    target_link_libraries(cami_redis_test PRIVATE cami_gateway_redis GTest::gtest GTest::gtest_main)
    add_test(NAME redis_test COMMAND cami_redis_test)
endif()

message(STATUS "=== CAMI Build Configuration ===")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Build Tests: ${CAMI_BUILD_TESTS}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
