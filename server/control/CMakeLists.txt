# server/control/CMakeLists.txt — TASK-040 ControlService（控制面）
#
# 依赖方向（§27.3）：server/control -> engine/core（error / bus / time）
#                                  -> server/dataservice（IDataStore 接口，持久化）
# 红线：公开头（include/）禁止 include 内部 src/；禁止拥有游戏实时状态；
#       禁止直连 MySQL / Redis（持久化必须经 IDataStore 接口）。

add_library(mmo_control STATIC
    src/control_service.cpp
    src/control_io.cpp)
add_library(mmo::control ALIAS mmo_control)

target_include_directories(mmo_control PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_include_directories(mmo_control PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(mmo_control PUBLIC cxx_std_20)
target_link_libraries(mmo_control PUBLIC
    mmo::core_error
    mmo::core_bus
    mmo::core_time
    mmo::dataservice
    mmo::gateway_router)
mmorpg_add_warnings(mmo_control)

# ---- 测试（§16 单元 / §17 集成 / §19 Failure，统一 Game_Control 前缀）----
# 验收脚本 run_ctest 'Control'：ctest -R Control 必须全部通过。
if(MMORPG_BUILD_TESTS)
    add_executable(control_test tests/control_test.cpp)
    target_link_libraries(control_test PRIVATE
        mmo::control
        mmo::gateway_router
        mmo::dataservice)
    target_include_directories(control_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    add_test(NAME Game_Control.Suite COMMAND control_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
