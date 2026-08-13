# gateway/transport/CMakeLists.txt — 传输抽象 + 连接迁移治理器 (spec §4.1 连接迁移模块)
# [PROTOTYPE] ITransport 抽象 / 仿真传输 / 迁移治理器 均为 header-only 纯 STL，OFF 可编译。
# QUIC 候选后端 [MODULES 门控]：find_package(msquic) 成功才编译真绑定（不破坏 OFF 构建）。

# 传输抽象 + 治理器（header-only INTERFACE lib，供网关 / 测试复用）。
add_library(cami_gateway_transport INTERFACE)
target_include_directories(cami_gateway_transport INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_compile_features(cami_gateway_transport INTERFACE cxx_std_17)

# QUIC 候选后端 [PROTOTYPE, MODULES 门控]
# 真编译需 msquic（在 vcpkg bootstrap 后安装）。find_package 守卫保证缺失时构建仍绿。
# vcpkg msquic 的 CMake 包名/target 名因上游 NAMESPACE 而异 (msquic / MsQuic::msquic / msquic::msquic),
# 用 if(TARGET) 探测第一个存在的, 保证任何 vcpkg 版本都能正确链接而非静默跳过。
if(CAMI_BUILD_MODULES)
    find_package(msquic CONFIG QUIET)
    if(NOT msquic_FOUND)
        find_package(MsQuic CONFIG QUIET)
    endif()
    set(CAMI_MSQUIC_TARGET "")
    foreach(_cand msquic::msquic MsQuic::msquic msquic)
        if(TARGET ${_cand})
            set(CAMI_MSQUIC_TARGET ${_cand})
            break()
        endif()
    endforeach()
    if(CAMI_MSQUIC_TARGET)
        add_library(cami_gateway_quic STATIC quic_transport.cpp)
        target_link_libraries(cami_gateway_quic PUBLIC cami_gateway_transport ${CAMI_MSQUIC_TARGET})
        target_compile_definitions(cami_gateway_quic PUBLIC CAMI_BUILD_MODULES)
        message(STATUS "[transport] QUIC 后端已启用 (msquic -> ${CAMI_MSQUIC_TARGET})")
    else()
        message(STATUS "[transport] msquic 未安装或 target 名未知, 跳过 QUIC 后端 (仅 ITransport 抽象 + 治理器)")
    endif()
endif()

# 连接迁移治理器单测 (OFF 可编译, 不依赖 QUIC)。
if(CAMI_BUILD_TESTS)
    add_executable(cami_connection_migrator_test ../../tests/unit/connection_migrator_test.cpp)
    target_link_libraries(cami_connection_migrator_test PRIVATE cami_gateway_transport)
    add_test(NAME connection_migrator_test COMMAND cami_connection_migrator_test)
endif()
