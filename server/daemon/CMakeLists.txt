# server/daemon/CMakeLists.txt — 四进程 daemon 入口（进程组合层，§3 Module ≠ Process）
#
# 背景：42 份任务书交付的是模块级产物（静态库 + 测试 + bench），进程层缺失导致
# deploy/docker 的 Dockerfile 引用不存在的二进制。本目录补齐四个可执行入口。
#
# 边界：daemon 只做「进程组装」（配置/日志/信号/主循环），不承载任何游戏语义；
# 只消费各模块公开接口（§27.3），禁止 include 任何模块 src/ 内部头。

# ---- 公共引导头（header-only，无独立编译单元）----
add_library(mmo_daemon_common INTERFACE)
add_library(mmo::daemon_common ALIAS mmo_daemon_common)
target_include_directories(mmo_daemon_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mmo_daemon_common INTERFACE cxx_std_20)
target_link_libraries(mmo_daemon_common INTERFACE
    mmo::core_config
    mmo::core_bus
    mmo::core_log)

# ---- gateway：TCP 监听 + Session 全生命周期 ----
add_executable(gateway gateway_main.cpp)
target_link_libraries(gateway PRIVATE
    mmo::daemon_common
    mmo::gateway_session
    mmo::gateway_router
    mmo::gateway_resilience
    mmo::net)
mmorpg_add_warnings(gateway)

# ---- smoke_client：gateway 端到端冒烟验证工具（非交付进程）----
add_executable(gateway_smoke_client smoke_client.cpp)
target_compile_features(gateway_smoke_client PRIVATE cxx_std_20)
if(WIN32)
    target_link_libraries(gateway_smoke_client PRIVATE ws2_32)
endif()

# ---- gamenode：20Hz 主循环 + 14 个游戏模块 ----
add_executable(gamenode gamenode_main.cpp)
target_link_libraries(gamenode PRIVATE
    mmo::daemon_common
    mmo::gamenode_entity
    mmo::gamenode_scene
    mmo::gamenode_sched
    mmo::gamenode_aoi
    mmo::gamenode_movement
    mmo::gamenode_role
    mmo::gamenode_inventory
    mmo::gamenode_ai
    mmo::gamenode_quest
    mmo::gamenode_world
    mmo::gamenode_combat
    mmo::gamenode_economy
    mmo::social
    mmo::scene_recovery)
mmorpg_add_warnings(gamenode)

# ---- dataservice：cache-aside + write-behind（内存实现，部署可替换）----
add_executable(dataservice dataservice_main.cpp)
target_link_libraries(dataservice PRIVATE
    mmo::daemon_common
    mmo::dataservice)
mmorpg_add_warnings(dataservice)

# ---- control：节点注册表 / 心跳超时 / 配置下发 ----
add_executable(control control_main.cpp)
target_link_libraries(control PRIVATE
    mmo::daemon_common
    mmo::control
    mmo::dataservice)
mmorpg_add_warnings(control)

# ---- mmorpg_server：All-in-One 单进程（开发/小规模形态；四角色各占一线程）----
# 与四个单进程 exe 复用同一份 RunXxx 实现：同一批源文件以 MMO_DAEMON_AS_LIBRARY
# 重编译一份 obj（剔除各自 main()），再链接 allinone_main.cpp 的 main。
add_executable(mmorpg_server allinone_main.cpp
    gateway_main.cpp gamenode_main.cpp dataservice_main.cpp control_main.cpp)
target_compile_definitions(mmorpg_server PRIVATE MMO_DAEMON_AS_LIBRARY)
foreach(_main IN ITEMS gateway_main gamenode_main dataservice_main control_main)
    set_source_files_properties(${_main}.cpp PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON)
endforeach()
target_link_libraries(mmorpg_server PRIVATE
    mmo::daemon_common
    mmo::gateway_session
    mmo::gateway_router
    mmo::gateway_resilience
    mmo::net
    mmo::gamenode_entity
    mmo::gamenode_scene
    mmo::gamenode_sched
    mmo::gamenode_aoi
    mmo::gamenode_movement
    mmo::gamenode_role
    mmo::gamenode_inventory
    mmo::gamenode_ai
    mmo::gamenode_quest
    mmo::gamenode_world
    mmo::gamenode_combat
    mmo::gamenode_economy
    mmo::social
    mmo::scene_recovery
    mmo::dataservice
    mmo::control)
mmorpg_add_warnings(mmorpg_server)

# 统一输出到 build/bin（与 test/bench 一致，Dockerfile 按此路径 COPY）
foreach(_d IN ITEMS gateway gamenode dataservice control mmorpg_server)
    set_target_properties(${_d} PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endforeach()
