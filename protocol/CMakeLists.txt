# protocol/CMakeLists.txt — TASK-005 Protocol Schema (Protobuf + FlatBuffers)
#
# 职责：
#   1. protoc / flatc 代码生成，产物输出到 ${CMAKE_BINARY_DIR}/generated/，
#      **禁止入库**（验收 §20.5：git status 不得出现 *.pb.h / *_generated.h）。
#   2. 构建 mmo_protocol 库（Envelope 编解码 + 校验器）。
#   3. 注册 ctest Protocol.Suite 与 bench/protocol.txt 压测。
#
# 命名空间契约（冻结，勿改）：
#   - schema/*.fbs（任务包冻结产物）：全局命名空间，flatc 风格枚举名
#     （::MessageBody_Command 等），file_identifier "MSG0"。
#   - movement/aoi/combat.fbs（本任务热路径帧）：namespace mmo.protocol。
#   - .proto：package mmo.protocol -> C++ namespace mmo::protocol。
#     视图层枚举用 EnvelopeMessageType（message_type.h），避免与生成的
#     mmo::protocol::MessageType 冲突。

# ---- 代码生成器定位 ------------------------------------------------------
# MSYS2 mingw64 提供 protoc.exe / flatc.exe；仓库 tools/flatc/ 为兜底副本。
find_program(PROTOC_EXECUTABLE NAMES protoc REQUIRED
    HINTS /mingw64/bin)
find_program(FLATC_EXECUTABLE NAMES flatc REQUIRED
    HINTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/flatc /mingw64/bin)

# ---- Protobuf 运行库 ----------------------------------------------------
# 优先 CONFIG 模式（MSYS2 提供 protobuf-config.cmake，target 自带 include 路径），
# 失败再退回 CMake 内置 FindProtobuf 模块。
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
    set(Protobuf_USE_STATIC_LIBS OFF)
    find_package(Protobuf MODULE REQUIRED)
endif()

set(PROTO_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(PROTO_GEN_DIR ${CMAKE_BINARY_DIR}/generated/protobuf)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

set(PROTO_FILES common.proto command.proto query.proto event.proto envelope.proto)
set(PB_CC "")
set(PB_HDRS "")

foreach(p ${PROTO_FILES})
    get_filename_component(n ${p} NAME_WE)
    list(APPEND PB_HDRS ${PROTO_GEN_DIR}/${n}.pb.h)
    list(APPEND PB_CC   ${PROTO_GEN_DIR}/${n}.pb.cc)
    add_custom_command(
        OUTPUT  ${PROTO_GEN_DIR}/${n}.pb.h ${PROTO_GEN_DIR}/${n}.pb.cc
        COMMAND ${PROTOC_EXECUTABLE}
                --proto_path=${PROTO_DIR}
                --cpp_out=${PROTO_GEN_DIR}
                ${PROTO_DIR}/${p}
        DEPENDS ${PROTO_DIR}/${p}
        COMMENT "protoc ${p}")
endforeach()

# ---- FlatBuffers 生成 ----------------------------------------------------
# -I 指向 schema/：热路径 movement/aoi/combat.fbs 的 `include "common.fbs"`
# 与冻结 schema 内部互相 include 都以此路径解析。
set(FBS_ROOT    ${CMAKE_CURRENT_SOURCE_DIR}/flatbuffers)
set(FBS_GEN_DIR ${CMAKE_BINARY_DIR}/generated/flatbuffers)
file(MAKE_DIRECTORY ${FBS_GEN_DIR})

set(FBS_FILES
    ${FBS_ROOT}/schema/common.fbs
    ${FBS_ROOT}/schema/envelope.fbs
    ${FBS_ROOT}/schema/command.fbs
    ${FBS_ROOT}/schema/query.fbs
    ${FBS_ROOT}/schema/event.fbs
    ${FBS_ROOT}/movement.fbs
    ${FBS_ROOT}/aoi.fbs
    ${FBS_ROOT}/combat.fbs
    ${FBS_ROOT}/envelope_transport.fbs)

set(FBS_HDRS "")
foreach(f ${FBS_FILES})
    get_filename_component(n ${f} NAME_WE)
    list(APPEND FBS_HDRS ${FBS_GEN_DIR}/${n}_generated.h)
    add_custom_command(
        OUTPUT  ${FBS_GEN_DIR}/${n}_generated.h
        COMMAND ${FLATC_EXECUTABLE} --cpp
                -I ${FBS_ROOT}/schema
                -o ${FBS_GEN_DIR}
                ${f}
        DEPENDS ${f}
        COMMENT "flatc ${n}.fbs")
endforeach()

# ---- 库目标：mmo_protocol ------------------------------------------------
# 依赖方向 protocol -> core_error（Result/Error），单向无环。
# 生成的 pb.cc 是第三方产物，-w 全关警告（禁止手改，见任务书 §21）。
add_library(mmo_protocol STATIC
    src/codec/owned_envelope.cpp
    src/codec/envelope_validator.cpp
    src/codec/protobuf_codec.cpp
    src/codec/flatbuf_codec.cpp
    ${PB_CC}
    ${PB_HDRS}
    ${FBS_HDRS})
add_library(mmo::protocol ALIAS mmo_protocol)

target_include_directories(mmo_protocol PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# 第三方生成物 / 第三方库头走 SYSTEM（-isystem），避免严格警告打在非本项目代码上
target_include_directories(mmo_protocol SYSTEM PUBLIC
    ${PROTO_GEN_DIR}
    ${FBS_GEN_DIR})
target_compile_features(mmo_protocol PUBLIC cxx_std_20)
target_link_libraries(mmo_protocol PUBLIC mmo::core_error)
if(TARGET protobuf::libprotobuf)
    target_link_libraries(mmo_protocol PUBLIC protobuf::libprotobuf)
else()
    target_include_directories(mmo_protocol SYSTEM PUBLIC ${Protobuf_INCLUDE_DIRS})
    target_link_libraries(mmo_protocol PUBLIC ${Protobuf_LIBRARIES})
endif()
mmorpg_add_warnings(mmo_protocol)

# 仅对 protoc 生成的 .cc 关警告（自研代码仍受完整严格警告约束）
if(PB_CC)
    set_source_files_properties(${PB_CC} PROPERTIES COMPILE_OPTIONS "-w")
endif()

# ---- 单元 / 集成 / Failure 测试（§16 / §17 / §19） ------------------------
if(MMORPG_BUILD_TESTS)
    add_executable(protocol_test tests/protocol_test.cpp)
    target_link_libraries(protocol_test PRIVATE mmo::protocol)
    # 复用 engine/core 的统一测试输出通道（红线：engine/ 禁 cout/printf）
    target_include_directories(protocol_test PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(protocol_test PRIVATE -Wno-pedantic)
    add_test(NAME Protocol.Suite COMMAND protocol_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- Benchmark（§18，输出 bench/protocol.txt key=value） ------------------
if(MMORPG_BUILD_BENCHMARKS)
    add_executable(protocol_bench tests/protocol_bench.cpp)
    target_link_libraries(protocol_bench PRIVATE mmo::protocol)
    target_include_directories(protocol_bench PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/core/tests)
    target_compile_options(protocol_bench PRIVATE -Wno-pedantic)
endif()
