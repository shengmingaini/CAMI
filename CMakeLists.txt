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

# === Build Acceleration & Memory Guardrails (CI OOM 防线) ===
# 1) ccache: 自动启用编译缓存。CI 跨 run 复用编译产物(含 protobuf 生成代码),
#    本地增量编译同样受益。需在第一个 target 前设置 CMAKE_CXX_COMPILER_LAUNCHER。
#    若系统无 ccache 或用户已显式指定 launcher, 则跳过, 不影响任何构建路径。
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}" CACHE FILEPATH "ccache compiler launcher")
    message(STATUS "ccache enabled: ${CCACHE_PROGRAM}")
endif()

# 2) Ninja job pools: 强制限制编译/链接并发, 防止链接期内存峰值吃爆 CI runner。
#    - link=1: 链接阶段串行, 峰值 = 单个最大 target 的内存 (gRPC/Protobuf 静态库链接
#      峰值 ~2-3GB, 串行后 16GB runner 绝对安全; 之前 Makefiles -j2 允许两个大 target
#      并行链接, 是 Week3/4 MODULES=ON job OOM 的主因之一)。
#    - compile=2: 编译并发封顶, 大型生成代码 (data_service.pb.cc 等) 单文件峰值已高,
#      双编译单元并行即可跑满 CPU 又不越内存。
#    仅对 Ninja 生成器生效 (Makefiles 忽略, 本地 MinGW 路径不受影响)。
if(CMAKE_GENERATOR STREQUAL "Ninja" AND NOT CMAKE_JOB_POOLS)
    set(CMAKE_JOB_POOLS "compile=2;link=1" CACHE STRING "Ninja job pools (compile/link concurrency caps)")
    set(CMAKE_JOB_POOL_COMPILE compile CACHE STRING "Ninja compile job pool")
    set(CMAKE_JOB_POOL_LINK link CACHE STRING "Ninja link job pool")
    message(STATUS "Ninja job pools: compile<=2, link=1 (OOM guardrail)")
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
# Gateway links OpenSSL (AES-GCM) + redis-plus-plus (RedisClusterState).
# D6 可选生产后端 (gRPC Data Service / Kafka / MySQL 落库) 由 data/CMakeLists.txt
# 以 find_package(... QUIET) + if(FOUND) 优雅引入: 装了的编译, 没装(MinGW 缺 libmysql 等)
# 跳过。故根处不再 REQUIRED 这些重型依赖, 避免本地子集安装时整体配置失败。
# vcpkg.json 已加回 grpc/protobuf/cppkafka/libmysql (重装会重新触发 ~20-40min 源码构建)。
if(CAMI_BUILD_MODULES)
    find_package(OpenSSL REQUIRED)
    # vcpkg port 名为 redis-plus-plus，但 CMake config 包名为 redis++（redis++Config.cmake）。
    find_package(redis++ CONFIG REQUIRED)
    # 全局透传：MODULES=ON 下各模块用 target_compile_definitions(PRIVATE ...) 仅作用于自身，
    # 测试可执行文件（cami_security_test / cami_redis_test）拿不到该宏，会导致
    # security_test.cpp 的 AES-GCM 真实路径 / redis_test.cpp 的 RedisCluster 真实测试被编译剔除。
    # 这里全局注入，保证 MODULES=ON 时所有目标（含测试）都看到 CAMI_BUILD_MODULES。
    add_compile_definitions(CAMI_BUILD_MODULES)
endif()

# === Include Directories ===
include_directories(${CMAKE_SOURCE_DIR})

# enable_testing 必须在 add_subdirectory 之前, 否则子目录里的 add_test 不注册
enable_testing()

# === Test dependencies (必须在 add_subdirectory 之前, 子目录测试需 GTest/Threads target) ===
if(CAMI_BUILD_TESTS)
    find_package(GTest REQUIRED)
    find_package(Threads REQUIRED)
endif()

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
add_executable(cami_skeleton_check tests/unit/skeleton_layer_check.cpp)
target_link_libraries(cami_skeleton_check PRIVATE cami_gateway cami_game cami_data cami_common cami_ops)
add_test(NAME skeleton_layer_check COMMAND cami_skeleton_check)

# === Tests (GTest-based unit/integration, 重型依赖就绪后开启) ===
if(CAMI_BUILD_TESTS)
    # GTest/Threads 已在 add_subdirectory 前引入 (子目录测试需其 target)。
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

    # Week3 集成层：四模块集成缝组合策略（限流/鉴权/选后端/在线态，确定性 selfcheck + GTest）。
    add_executable(cami_integration_test tests/unit/gateway_pipeline_test.cpp)
    target_link_libraries(cami_integration_test PRIVATE cami_gateway_integration GTest::gtest GTest::gtest_main)
    add_test(NAME integration_test COMMAND cami_integration_test)

    # 内容模块化骨架：注册表/依赖拓扑/双闸门/分阶段等级上限/配置 LRU/Lua 懒加载 (纯 STL)。
    add_executable(cami_content_test tests/unit/content_module_test.cpp)
    target_link_libraries(cami_content_test PRIVATE cami_game GTest::gtest GTest::gtest_main)
    add_test(NAME content_module_test COMMAND cami_content_test)

    # 阶段 A 基础设施：Arena 分配器 + SPMC 无锁队列 (纯 header, common 层)
    add_executable(cami_arena_spmc_test tests/unit/arena_spmc_test.cpp)
    target_link_libraries(cami_arena_spmc_test PRIVATE cami_common GTest::gtest GTest::gtest_main)
    target_link_libraries(cami_arena_spmc_test PRIVATE Threads::Threads)  # 多消费者测试
    add_test(NAME arena_spmc_test COMMAND cami_arena_spmc_test)
endif()

message(STATUS "=== CAMI Build Configuration ===")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Build Tests: ${CAMI_BUILD_TESTS}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
