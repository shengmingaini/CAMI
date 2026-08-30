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

# ---- 库目标：TASK-003 Core Time（MonotonicClock / WallClock / TickClock） ----
# 依赖方向 time -> error（Result/Error 表达校验失败），单向无环。
# timer.h 只定义 ITimerQueue 接口、不实现调度（留给 TASK-004 Scheduler），无需编译单元。
add_library(mmo_core_time STATIC
    src/time/clock.cpp
    src/time/wall_clock.cpp
    src/time/tick_clock.cpp
)
add_library(mmo::core_time ALIAS mmo_core_time)

target_include_directories(mmo_core_time PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
# 内部头（墙钟注入测试缝）只对本库与同模块白盒测试可见，不进 PUBLIC 接口
target_include_directories(mmo_core_time PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(mmo_core_time PUBLIC cxx_std_20)
target_link_libraries(mmo_core_time PUBLIC mmo::core_error)
mmorpg_add_warnings(mmo_core_time)

# ---- 库目标：TASK-003 Core UUID ----
# 依赖方向 uuid -> time（V7 需要 WallClock::UnixMillis）+ uuid -> error，单向无环。
# thread_local 熵缓冲，无全局锁、无共享随机引擎（任务书 §21）。
add_library(mmo_core_uuid STATIC
    src/uuid/uuid.cpp
    src/uuid/entropy.cpp
)
add_library(mmo::core_uuid ALIAS mmo_core_uuid)

target_include_directories(mmo_core_uuid PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
# 内部头（熵源测试注入缝）只对本库与同模块白盒测试可见，不进 PUBLIC 接口
target_include_directories(mmo_core_uuid PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(mmo_core_uuid PUBLIC cxx_std_20)
target_link_libraries(mmo_core_uuid PUBLIC mmo::core_error mmo::core_time)
# Windows CNG：BCryptGenRandom 需要 bcrypt 导入库（MinGW 自带 libbcrypt.a）
if(WIN32)
    target_link_libraries(mmo_core_uuid PRIVATE bcrypt)
endif()
mmorpg_add_warnings(mmo_core_uuid)

# ---- 库目标：TASK-003 Core Config（JSON 配置中心） ----
# 依赖方向 config -> error，单向无环；不依赖 log（避免 core 内部不必要的耦合）。
# 内部头（极简 JSON 解析器 / 不可变快照定义）只对本库与同模块白盒测试可见。
add_library(mmo_core_config STATIC
    src/config/json_parser.cpp
    src/config/config_manager.cpp
)
add_library(mmo::core_config ALIAS mmo_core_config)

target_include_directories(mmo_core_config PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(mmo_core_config PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(mmo_core_config PUBLIC cxx_std_20)
target_link_libraries(mmo_core_config PUBLIC mmo::core_error)
mmorpg_add_warnings(mmo_core_config)

# ---- 库目标：TASK-004 Core Thread（TaskFn / MpmcQueue / Thread） ----
# 依赖方向 thread -> error（Result/Error 表达 BUSY / INVALID_ARGUMENT），单向无环。
# 注意：sched/ 与 memory/ 是本任务另外两个独立子库，见下方。
add_library(mmo_core_thread STATIC
    src/thread/thread.cpp
)
add_library(mmo::core_thread ALIAS mmo_core_thread)

target_include_directories(mmo_core_thread PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(mmo_core_thread PUBLIC cxx_std_20)
target_link_libraries(mmo_core_thread PUBLIC mmo::core_error)
mmorpg_add_warnings(mmo_core_thread)

# ---- 库目标：TASK-004 Core Scheduler（最小堆定时器，宿主驱动） ----
# 依赖方向 sched -> error + time（TickClock 的 SteadyTime / MonotonicClock），单向无环。
# 红线：本目录禁止出现 std::thread（Scheduler 不自带线程，验收脚本静态扫描）。
add_library(mmo_core_sched STATIC
    src/sched/scheduler.cpp
)
add_library(mmo::core_sched ALIAS mmo_core_sched)

target_include_directories(mmo_core_sched PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(mmo_core_sched PUBLIC cxx_std_20)
target_link_libraries(mmo_core_sched PUBLIC mmo::core_error mmo::core_time)
mmorpg_add_warnings(mmo_core_sched)

# ---- 库目标：TASK-004 Core Memory（ObjectPool / MemoryPool / Arena） ----
# 三个设施都是「单线程拥有」（§4 State Owner），热路径无锁；
# ObjectPool 是纯模板（header-only），只有 MemoryPool / Arena 有编译单元。
add_library(mmo_core_memory STATIC
    src/memory/memory_pool.cpp
    src/memory/arena.cpp
)
add_library(mmo::core_memory ALIAS mmo_core_memory)

target_include_directories(mmo_core_memory PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(mmo_core_memory PUBLIC cxx_std_20)
target_link_libraries(mmo_core_memory PUBLIC mmo::core_error)
mmorpg_add_warnings(mmo_core_memory)

# ---- 库目标：TASK-007 Core Bus（CommandBus / QueryBus / EventBus） ----
# 依赖方向 bus -> error（Result/Error）+ log（TraceID/RequestID/PlayerID/SceneID）
#              + thread（MpmcQueue，事件入队）+ time（MonotonicClock，Drain 时间预算），单向无环。
# 红线：src/bus 内禁止出现 std::thread（EventBus 必须宿主 Drain，验收脚本静态扫描）。
# 刻意**不**依赖 protocol：命令上下文的 trace/request id 复用 core 内既有 ID 语义，
# 以保证 Core 不反向依赖上层模块（理由见 engine/core/docs/DEPENDENCY.md 第五章）。
add_library(mmo_core_bus STATIC
    src/bus/command.cpp
    src/bus/command_bus.cpp
    src/bus/query_bus.cpp
    src/bus/event_bus.cpp
)
add_library(mmo::core_bus ALIAS mmo_core_bus)

target_include_directories(mmo_core_bus PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(mmo_core_bus PUBLIC cxx_std_20)
target_link_libraries(mmo_core_bus PUBLIC
    mmo::core_error mmo::core_log mmo::core_thread mmo::core_time)
mmorpg_add_warnings(mmo_core_bus)

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

# ---- 单元测试：TASK-003 Core Time / UUID / Config ----
# 一个可执行文件覆盖 §16 单元 / §17 集成 / §19 Failure，统一以 Core_Time 前缀注册，
# 使验收脚本的 `ctest -R Core_Time` 能真正跑完全部 TASK-003 用例。
if(MMORPG_BUILD_TESTS)
    add_executable(core_time_test tests/time_test.cpp)
    target_link_libraries(core_time_test PRIVATE mmo::core_time mmo::core_uuid mmo::core_config)
    # 白盒访问本模块内部头（墙钟注入缝 / 熵源注入缝），不进 PUBLIC 接口
    target_include_directories(core_time_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_options(core_time_test PRIVATE -Wno-pedantic)
    # 工作目录必须是仓库根：测试要加载真实 config/ 目录（§20.6 要求至少 3 份配置可被加载）。
    # ctest 默认在 build 目录下执行，不加这句 LoadDir("config") 一定找不到路径。
    add_test(NAME Core_Time.Suite COMMAND core_time_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # TASK-004：Memory / Thread / Scheduler 单元 + 集成 + Failure 测试，统一以 Core_Thread 前缀注册
    add_executable(core_thread_test tests/thread_test.cpp)
    target_link_libraries(core_thread_test PRIVATE
        mmo::core_memory mmo::core_thread mmo::core_sched
        mmo::core_time mmo::core_error)
    # 白盒访问本模块内部头（如需），不进 PUBLIC 接口
    target_include_directories(core_thread_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(core_thread_test PRIVATE -Wno-pedantic)
    add_test(NAME Core_Thread.Suite COMMAND core_thread_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # TASK-007：Command / Query / Event Bus —— 单元（§16）+ 集成（§17）+ Failure（§19），
    # 统一以 Core_Bus 前缀注册，使验收脚本的 `ctest -R Core_Bus` 能跑完全部用例。
    add_executable(core_bus_test tests/bus_test.cpp)
    target_link_libraries(core_bus_test PRIVATE mmo::core_bus)
    target_compile_options(core_bus_test PRIVATE -Wno-pedantic)
    add_test(NAME Core_Bus.Suite COMMAND core_bus_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # TASK-007 §15.8 Demo 全链路：Command → Handler → Event → 2 × Subscriber
    add_executable(core_bus_demo tests/demo_pipeline.cpp)
    target_link_libraries(core_bus_demo PRIVATE mmo::core_bus)
    target_compile_options(core_bus_demo PRIVATE -Wno-pedantic)
    add_test(NAME Core_Bus.Demo COMMAND core_bus_demo
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（1e7 次，输出 bench/core_error.txt 的 key=value） ----
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(error_bench tests/error_bench.cpp)
    target_link_libraries(error_bench PRIVATE mmo::core_error)
    target_compile_options(error_bench PRIVATE -Wno-pedantic)

    # TASK-002：8 线程 × 10 万条压测，输出 bench/core_log.txt
    add_executable(log_bench tests/log_bench.cpp)
    target_link_libraries(log_bench PRIVATE mmo::core_log)

    # TASK-003：单调时钟 / UUID / Config Get 压测，输出 bench/core_time.txt
    add_executable(time_bench tests/time_bench.cpp)
    target_link_libraries(time_bench PRIVATE mmo::core_time mmo::core_uuid mmo::core_config)
    target_compile_options(time_bench PRIVATE -Wno-pedantic)

    # TASK-004：Scheduler 每 Tick 定时器扫描 + Memory 三件套吞吐
    add_executable(sched_bench tests/sched_bench.cpp)
    target_link_libraries(sched_bench PRIVATE mmo::core_sched mmo::core_time)
    target_compile_options(sched_bench PRIVATE -Wno-pedantic)

    add_executable(mem_bench tests/mem_bench.cpp)
    target_link_libraries(mem_bench PRIVATE mmo::core_memory mmo::core_time)
    target_compile_options(mem_bench PRIVATE -Wno-pedantic)

    # TASK-007：1e6 次 Command Dispatch + 1e6 次 Event Publish/Drain，
    # 输出 bench/core_bus.txt（供验收脚本 assert_metric 解析）。
    add_executable(bus_bench tests/bus_bench.cpp)
    target_link_libraries(bus_bench PRIVATE mmo::core_bus)
    target_compile_options(bus_bench PRIVATE -Wno-pedantic)
endif()
