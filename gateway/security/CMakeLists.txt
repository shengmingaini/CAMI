# gateway/security/CMakeLists.txt — 安全校验模块 (spec §3.1 / §4.1)
# 消息 AES-GCM 加解密 + 登录 token 鉴权。
#
# 加密依赖 OpenSSL，门控在 CAMI_BUILD_MODULES=ON（vcpkg.json 已声明 openssl）。
# CAMI_BUILD_MODULES=OFF：模块仍编译，但 aes_gcm 为 DISABLED 占位实现（selfcheck 标记跳过），
#   符合"轻量 CI 仅 Boost、重型依赖默认关闭"的项目规则；token 鉴权不依赖 OpenSSL，始终可测。
# 依赖下层：无（payload 透传，加解密在模块内闭环）。
add_library(cami_gateway_security STATIC
    token_auth.cpp
    security_selfcheck.cpp
    aes_gcm.cpp
)
target_include_directories(cami_gateway_security PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(cami_gateway_security PUBLIC cxx_std_17)

if(CAMI_BUILD_MODULES)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(cami_gateway_security PRIVATE OpenSSL::Crypto)
    target_compile_definitions(cami_gateway_security PRIVATE CAMI_BUILD_MODULES)
endif()
