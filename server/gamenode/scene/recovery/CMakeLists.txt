# server/gamenode/scene/recovery/CMakeLists.txt — TASK-037 §37.4 Scene Recovery
#
# 新增子模块（非修改 TASK-012 既有目标）：Scene 恢复第一版（Checkpoint + 新节点重建）。
# 仅消费 TASK-012 Scene / SceneManager 公开接口（§27.3），不 include 任何 src/。

add_library(mmo_scene_recovery STATIC
    ../src/recovery/scene_recovery.cpp)
add_library(mmo::scene_recovery ALIAS mmo_scene_recovery)

target_include_directories(mmo_scene_recovery PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/server/gamenode/scene/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(mmo_scene_recovery PUBLIC cxx_std_20)
target_link_libraries(mmo_scene_recovery PUBLIC
    mmo::core_error
    mmo::core_bus
    mmo::core_time
    mmo::gamenode_scene)   # 消费 Scene / SceneManager 公开接口
mmorpg_add_warnings(mmo_scene_recovery)

# ---- 测试（§16 单元 / §19 Failure）----
# 验收脚本 run_ctest 'Resilience'：ctest -R Resilience 必须覆盖本测试。
if(MMORPG_BUILD_TESTS)
    add_executable(scene_recovery_test
        ${CMAKE_SOURCE_DIR}/server/gamenode/scene/tests/scene_recovery_test.cpp)
    target_link_libraries(scene_recovery_test PRIVATE
        mmo::scene_recovery
        mmo::gamenode_scene)
    target_include_directories(scene_recovery_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(scene_recovery_test PRIVATE -Wno-pedantic)
    add_test(NAME Game_SceneRecovery.Resilience.Suite COMMAND scene_recovery_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
