# data/data_service/CMakeLists.txt — gRPC Data Service (仅 CAMI_BUILD_MODULES=ON)
# 由 data/CMakeLists.txt 的 `if(CAMI_BUILD_MODULES) add_subdirectory(data_service)` 调用。
#
# 用 protoc 生成 data_service.pb.{h,cc} 与 data_service.grpc.pb.{h,cc},
# 编入 cami_data_service 静态库, 链接 grpc++ / protobuf。

# 缺 protobuf/grpc 时优雅跳过 (本地 MinGW 未装这些重依赖; CI(Linux) 全量安装后正常编译)。
find_package(Protobuf CONFIG QUIET)
find_package(gRPC CONFIG QUIET)
if(NOT Protobuf_FOUND OR NOT gRPC_FOUND)
    message(WARNING "[data_service] Protobuf/gRPC 未安装, 跳过 Data Service 构建 (CI 由 vcpkg 安装 protobuf grpc 后正常编译)")
    return()
endif()

# protoc 与 grpc 插件可执行文件
find_program(PROTOC_EXECUTABLE protoc)
find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)
if(NOT PROTOC_EXECUTABLE)
    message(FATAL_ERROR "[data_service] protoc not found")
endif()
if(NOT GRPC_CPP_PLUGIN_EXECUTABLE)
    message(FATAL_ERROR "[data_service] grpc_cpp_plugin not found")
endif()

set(PROTO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../proto/data_service.proto)
set(GEN_DIR  ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN_DIR})

# 代码生成: pb + grpc
add_custom_command(
    OUTPUT ${GEN_DIR}/data_service.pb.h ${GEN_DIR}/data_service.pb.cc
           ${GEN_DIR}/data_service.grpc.pb.h ${GEN_DIR}/data_service.grpc.pb.cc
    COMMAND ${PROTOC_EXECUTABLE}
        --cpp_out=${GEN_DIR}
        --grpc_out=${GEN_DIR}
        --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
        -I ${CMAKE_CURRENT_SOURCE_DIR}/../../proto
        ${PROTO_SRC}
    DEPENDS ${PROTO_SRC}
    COMMENT "Generating gRPC code for data_service.proto"
)

add_library(cami_data_service STATIC
    data_service_impl.cpp
    data_client.cpp
    ${GEN_DIR}/data_service.pb.cc
    ${GEN_DIR}/data_service.grpc.pb.cc
)
target_include_directories(cami_data_service PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${GEN_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../..     # 暴露 data/redis_proxy/cache_proxy.h 等
)
target_compile_features(cami_data_service PUBLIC cxx_std_17)
target_link_libraries(cami_data_service PUBLIC
    cami_data
    gRPC::gRPC++
    protobuf::libprotobuf
)

# 可选: Data Service 独立进程 (部署用)
add_executable(cami_data_service_main data_service_main.cpp)
target_link_libraries(cami_data_service_main PRIVATE cami_data_service)
