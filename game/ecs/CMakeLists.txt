# game/ecs — ECS 核心框架 (阶段 A W3)
# components.h : 纯 POD 组件, OFF 可编可测 (SoA 友好, 零外部依赖).
# ecs_world.h : Entt 封装, 门控 CAMI_BUILD_MODULES (Entt 由 vcpkg 提供).

# ---- 组件层单测 (始终编译, OFF 亦可) ----
if(CAMI_BUILD_TESTS)
    add_executable(cami_ecs_component_test ../../tests/unit/ecs_component_test.cpp)
    target_include_directories(cami_ecs_component_test PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_features(cami_ecs_component_test PRIVATE cxx_std_17)
    target_link_libraries(cami_ecs_component_test PRIVATE GTest::gtest GTest::gtest_main)
    add_test(NAME ecs_component_test COMMAND cami_ecs_component_test)
endif()

# ---- Entt 世界封装 (门控, MODULES=ON) ----
if(CAMI_BUILD_MODULES)
    find_package(EnTT CONFIG REQUIRED)
    add_library(cami_ecs INTERFACE)
    target_include_directories(cami_ecs INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cami_ecs INTERFACE EnTT::EnTT)

    if(CAMI_BUILD_TESTS)
        add_executable(cami_ecs_world_test ../../tests/unit/ecs_world_test.cpp)
        target_include_directories(cami_ecs_world_test PRIVATE ${CMAKE_SOURCE_DIR})
        target_compile_features(cami_ecs_world_test PRIVATE cxx_std_17)
        target_link_libraries(cami_ecs_world_test PRIVATE cami_ecs GTest::gtest GTest::gtest_main)
        add_test(NAME ecs_world_test COMMAND cami_ecs_world_test)
    endif()
endif()
