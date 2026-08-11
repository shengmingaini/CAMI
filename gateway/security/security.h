#pragma once

#include "gateway/security/security_types.h"
#include "gateway/security/aes_gcm.h"
#include "gateway/security/token_auth.h"

// 安全校验模块统一包含头。
// 注意：AesGcmCipher 的真实实现（OpenSSL）仅在 CAMI_BUILD_MODULES=ON 下可用；
// OFF 构建下 encrypt/decrypt 返回 false（kErrDisabled）。TokenAuth 不依赖 OpenSSL。
