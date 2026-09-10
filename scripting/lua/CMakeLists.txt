# scripting/lua/CMakeLists.txt — TASK-031 Lua Runtime
#
# 依赖方向（§27.3）：scripting/lua -> engine/core（error / error_code / result / time / bus）
#                                  -> server/gamenode/entity（EntityManager，§27.2 消费公开接口）
#
# Lua 来源（§15.1「引入 Lua，锁定版本，产物不入库」）
# ---------------------------------------------------
# 走 **MSYS2 MinGW64 系统包**，与 protocol 模块消费 MSYS2 protobuf/flatbuffers 的口径一致
# （见 protocol/CMakeLists.txt）。理由：
#   1. 本机 vcpkg 未安装 lua，走 vcpkg manifest 需要联网拉源码，而企业代理下单点限速不稳定；
#   2. MSYS2 已提供与 MinGW 工具链**同源**的 lua 导入库（liblua.dll.a）+ lua.pc，
#      版本可与 vcpkg baseline 一样被锁定（当前实测 Lua 5.5.1，见构建输出）；
#   3. 运行期只需 lua55.dll 在 PATH 上（验收脚本已注入 /c/msys64/mingw64/bin）。
# 构建产物（liblua.dll.a / lua55.dll）**不入库**：它们是外部依赖，不是本仓库源码产物。
#
# 如需切到 vcpkg：在 vcpkg.json 的 dependencies 加 "lua"，并把下方 find_package 保持原样
# （CMake 的 FindLua 会优先命中 vcpkg 提供的包）。

# ---- Lua 定位 ----
# 兼容性坑（实测 CMake 4.4 + MSYS2 lua 5.5.1）：FindLua 只设置了**单数** LUA_INCLUDE_DIR，
# 复数 LUA_INCLUDE_DIRS 为空。若直接用 ${LUA_INCLUDE_DIRS}，lua.h 会找不到 → 编译失败。
find_package(Lua REQUIRED)

set(MMORPG_LUA_INCLUDE_DIRS "${LUA_INCLUDE_DIRS}")
if(NOT MMORPG_LUA_INCLUDE_DIRS)
    set(MMORPG_LUA_INCLUDE_DIRS "${LUA_INCLUDE_DIR}")
endif()
if(NOT MMORPG_LUA_INCLUDE_DIRS)
    message(FATAL_ERROR "找到 Lua 但拿不到头文件目录（LUA_INCLUDE_DIR / LUA_INCLUDE_DIRS 均为空）")
endif()
message(STATUS "MMORPG Lua: version=${LUA_VERSION_STRING} include=${MMORPG_LUA_INCLUDE_DIRS}")

# 把 Lua 头作为 SYSTEM 引入：抑制第三方头内部的告警，保持本模块「零告警」口径干净。
add_library(mmo_lua_thirdparty INTERFACE)
target_include_directories(mmo_lua_thirdparty SYSTEM INTERFACE ${MMORPG_LUA_INCLUDE_DIRS})
target_link_libraries(mmo_lua_thirdparty INTERFACE ${LUA_LIBRARIES})

# ---- 本模块库 ----
add_library(mmo_script_lua STATIC
    src/lua_vm.cpp
    src/sandbox.cpp
    src/script_value.cpp
    src/script_context.cpp
    src/bind_entity.cpp
    src/bind_command.cpp
    src/bind_query.cpp
    src/bind_event.cpp
    # ---- TASK-032 · Lua Hot Reload ----
    src/hot_reload/hot_reloader.cpp
    src/hot_reload/audit_sink.cpp
    src/hot_reload/script_reload_stage.cpp)
add_library(mmo::script_lua ALIAS mmo_script_lua)

target_include_directories(mmo_script_lua PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_script_lua PUBLIC cxx_std_20)
target_link_libraries(mmo_script_lua PUBLIC
    mmo::core_error
    mmo::core_bus
    mmo::core_time
    mmo::gamenode_entity
    # TASK-032 §27.2：消费 TASK-013 的公开接口（ISimulationStage / TickPhase / SceneContext），
    # 用于把热更安全点接到 Tick 边界。方向为 scripting -> scheduler（无环：scheduler 不依赖
    # scripting，scene/entity 也不依赖 scripting，已核对）。
    mmo::gamenode_sched
    mmo_lua_thirdparty)
mmorpg_add_warnings(mmo_script_lua)

# ---- 测试（§16 单元 / §17 集成 / §19 Failure）----
# 验收脚本 run_ctest 'Lua'：ctest -R Lua 必须全部通过。
# 注意：模式 'Lua' 必须能匹配到**至少**一个用例，否则验收脚本会拦下「零用例假通过」。
if(MMORPG_BUILD_TESTS)
    add_executable(lua_test tests/lua_test.cpp)
    target_link_libraries(lua_test PRIVATE mmo::script_lua)
    target_include_directories(lua_test PRIVATE ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(lua_test PRIVATE -Wno-pedantic)
    add_test(NAME Lua.Suite COMMAND lua_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    add_executable(lua_integration_test tests/lua_integration_test.cpp)
    target_link_libraries(lua_integration_test PRIVATE mmo::script_lua)
    target_include_directories(lua_integration_test PRIVATE ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(lua_integration_test PRIVATE -Wno-pedantic)
    add_test(NAME Lua_Integration.Suite COMMAND lua_integration_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # ---- TASK-032 · Lua Hot Reload（验收脚本 run_ctest 'HotReload'）----
    add_executable(hot_reload_test tests/hot_reload_test.cpp)
    target_link_libraries(hot_reload_test PRIVATE mmo::script_lua)
    target_include_directories(hot_reload_test PRIVATE ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(hot_reload_test PRIVATE -Wno-pedantic)
    add_test(NAME HotReload.Suite COMMAND hot_reload_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    add_executable(hot_reload_integration_test tests/hot_reload_integration_test.cpp)
    target_link_libraries(hot_reload_integration_test PRIVATE mmo::script_lua)
    target_include_directories(hot_reload_integration_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(hot_reload_integration_test PRIVATE -Wno-pedantic)
    add_test(NAME HotReload_Integration.Suite COMMAND hot_reload_integration_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18，lua_bench 输出 bench/lua.txt）----
# 验收脚本：run_bench bin/lua_bench --calls 1000000
#           assert_metric bench/lua.txt lua_call_ns le 2000
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(lua_bench benchmark/lua_bench.cpp)
    target_link_libraries(lua_bench PRIVATE mmo::script_lua)
    target_include_directories(lua_bench PRIVATE ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(lua_bench PRIVATE -Wno-pedantic)
    set_target_properties(lua_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)

    # ---- TASK-032 · Lua Hot Reload（验收脚本：bin/hotreload_bench --scripts 100 --reloads 10）----
    add_executable(hotreload_bench benchmark/hotreload_bench.cpp)
    target_link_libraries(hotreload_bench PRIVATE mmo::script_lua)
    target_include_directories(hotreload_bench PRIVATE ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(hotreload_bench PRIVATE -Wno-pedantic)
    set_target_properties(hotreload_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()

# ---- 运维 CLI（§15 第 9 步 / §23 Deliverables: tools/scriptctl/*）----
# 接入方式与 tools/migrate/main.cpp → mysql_migrate 一致（源码放 tools/，由所属模块接入 CMake）。
# 刻意放在 MMORPG_BUILD_TESTS/BENCHMARKS 之外：这是运维/CI 门禁工具，常规构建也应产出。
add_executable(scriptctl ${CMAKE_SOURCE_DIR}/tools/scriptctl/main.cpp)
target_link_libraries(scriptctl PRIVATE mmo::script_lua)
target_compile_options(scriptctl PRIVATE -Wno-pedantic)
set_target_properties(scriptctl PROPERTIES RUNTIME_OUTPUT_DIRECTORY
    ${CMAKE_BINARY_DIR}/bin)
