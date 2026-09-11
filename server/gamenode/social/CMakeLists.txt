# server/gamenode/social/CMakeLists.txt — TASK-039 Social System
#
# 新增子模块（非修改既有任务子树）。仅消费上游公开接口（§27.3）：
#   mmo::core_error / core_bus / core_time / dataservice / gamenode_scene / gamenode_entity
# 禁止 #include 任何上游 src/；禁止直改 Role / Scene 私有数据。

add_library(mmo_social STATIC
    src/social_system.cpp
    src/social_io.cpp
    src/social_config.cpp)
add_library(mmo::social ALIAS mmo_social)

target_include_directories(mmo_social PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_include_directories(mmo_social PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)   # 内部序列化头供本模块 src 消费
target_compile_features(mmo_social PUBLIC cxx_std_20)
target_link_libraries(mmo_social PUBLIC
    mmo::core_error
    mmo::core_bus
    mmo::core_time
    mmo::dataservice
    mmo::gamenode_scene
    mmo::gamenode_entity)
mmorpg_add_warnings(mmo_social)

# ---- 测试（§16 单元 / §17 集成 / §19 Failure）----
# 验收脚本 run_ctest 'Social'：ctest -R Social 必须全部通过。
if(MMORPG_BUILD_TESTS)
    add_executable(social_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/social_test.cpp)
    target_link_libraries(social_test PRIVATE
        mmo::social
        mmo::dataservice)
    target_include_directories(social_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/src)   # 允许测试消费内部序列化头
    target_compile_options(social_test PRIVATE -Wno-pedantic)
    add_test(NAME Game_Social.Suite COMMAND social_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
