# benchmark/resilience/CMakeLists.txt — TASK-037 §18 Benchmark（跨模块容灾）
#
# 消费 server/gateway（resilience：37.1/37.2/37.3）与 server/gamenode/scene（37.4）的公开接口。
# benchmark/ 在根 CMake 循环中最后处理，故上述目标均已定义，可安全链接。

if(MMORPG_BUILD_BENCHMARKS)
    add_executable(resilience_bench
        resilience_bench.cpp)
    target_link_libraries(resilience_bench PRIVATE
        mmo::gateway_resilience      # HealthMonitor / ReconnectService / FailoverCoordinator
        mmo::scene_recovery          # SceneRecovery（37.4）
        mmo::gamenode_scene          # Scene / SceneManager（37.4 真实重建）
        mmo::gamenode_entity         # EntityManager（SceneManager 依赖）
        mmo::core_error
        mmo::core_bus
        mmo::core_time)
    target_include_directories(resilience_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    set_target_properties(resilience_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_BINARY_DIR}/bin)
endif()
