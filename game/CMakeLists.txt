# game/CMakeLists.txt — 逻辑业务层 (spec §3.1)
# 场景调度 / 角色 / 战斗 / AOI / 社交 / 任务 / 交易经济 / 寻路 / 碰撞 / 预测 / 事件总线。
# 依赖下层：common + data。
add_library(cami_game STATIC
    game_layer.cpp
)
target_include_directories(cami_game PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(cami_game PUBLIC cxx_std_17)
target_link_libraries(cami_game PUBLIC cami_common cami_data)
