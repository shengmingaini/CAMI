# 安全校验模块详细设计

> **文档状态**: [PROTOTYPE]  
> **版本**: v1.0.0  
> **更新日期**: 2026-08-24 (Week3 周一 — 安全校验)  
> **所属层**: 接入层（gateway）  
> **上游规约**: `docs/architecture/architecture-spec.md`  
> **关联 ADR**: ADR-002（模块边界）

---

## 1. 模块概述

- **定位**：网关接入层的安全边界——消息加密（AES-256-GCM）与登录 token 鉴权。
- **核心职责**：
  1. 对客户端上行/下行消息体做 AES-256-GCM 加解密（机密性 + 完整性 + 防篡改）。
  2. 登录 token 签发 / 校验 / 吊销（网关侧，不透明 token）。
- **不在职责内（边界）**：限流/防攻击（Day2 ratelimit 模块）、路由（Day3 router）、在线态存储后端（Day4 Redis）。本模块只提供 token 的**接口与内存版实现**，Redis 后端为预留缝。

## 2. 架构约束与边界

- 是否拥有 `Player` 对象：否。token 仅携带 `player_id`，不触碰角色字段。
- 加密依赖 OpenSSL，门控在 `CAMI_BUILD_MODULES=ON`（vcpkg 已声明 `openssl`）。
- `CAMI_BUILD_MODULES=OFF`（轻量 CI）下 `AesGcmCipher` 为 DISABLED 占位实现，`security_selfcheck` 标记 `[DISABLED]` 但仍通过；token 鉴权不依赖 OpenSSL，始终可测。

## 3. 对外接口（C++ 签名级）

```cpp
// 加解密（AES-256-GCM）
class AesGcmCipher {
    static bool encrypt(key, plaintext, nonce_out, ciphertext_out);   // 布局: ct||tag(16)
    static bool decrypt(key, nonce, ciphertext_with_tag, plaintext_out); // 标签校验失败返回 false
};

// 登录 token 鉴权
class TokenAuth {
    std::string issue(uint64_t player_id);            // 签发不透明 token
    TokenVerifyResult verify(const std::string&);     // {status, player_id}
    void revoke(const std::string&);                  // 吊销（预留分布式吊销）
};
```

## 4. 核心数据结构

- `SecResult`：统一结果码（kOk / kErrDisabled / kErrEncrypt / kErrDecrypt / kErrToken*）。
- `SecurityConfig`：AES 密钥（32B）+ token TTL。
- `TokenAuth::Entry { player_id, expiry_ms }`：内存存储条目（周四替换为 Redis）。

## 5. 协议引用

- 加密作用于消息体（codec 帧的 payload 段），于 codec 之后、路由之前插入。
- token 由登录流程下发，网关在每个受保护消息上校验；校验失败直接断连。

## 6. 性能预算（验收）

| 指标 | 红线 | 本模块保障 |
|------|------|-----------|
| 加解密耗时 | < 0.05ms/包 | OpenSSL AES-NI 实测（见 `benchmark/security_bench.cpp`，`MODULES=ON` 下运行） |

> 当前轻量 CI（OFF）不编译真实加密，故 <0.05ms 验收在 `MODULES=ON` + OpenSSL 下实测（类 `gateway_bandwidth_bench` 微基准）。

## 7. 并发模型

- `AesGcmCipher` 无状态、线程安全（每次调用新建 EVP_CTX）。
- `TokenAuth` 内存版 `store_` 未加锁（原型单线程鉴权路径）；周四 Redis 版为线程安全后端。

## 8. 依赖方向

```
[连接处理] ──→ [security] ──→ [OpenSSL (MODULES=ON)]
                │
                └─→ [TokenStore 接口] ──(周四)──→ [Redis]
```

## 9. 关联 ADR / 架构章节

| 决策 | 编号 |
|------|------|
| 模块边界 WoW 模式 | ADR-002 |
| 重型依赖默认关闭 | 项目构建约定（`CAMI_BUILD_MODULES`） |

## 10. 开放问题 / 后续

- **密钥管理**：原型用固定测试密钥；生产须来自 KMS / 配置中心，禁止硬编码（安全红线）。
- **Redis 后端**：周四接入，替换 `TokenAuth::store_`，实现在线态 + 分布式吊销。
- **集成缝**：`AesGcmCipher` 与 `TokenAuth` 已在 `gateway/connection` 的 on_data 路径预留调用点（不改 Connection 本体，遵循"后期模块不 mutate 早期模块"纪律）。
- **CI 覆盖**：真实加密路径（MODULES=ON）当前不进轻量 CI；与其他重型模块一致，需 vcpkg 工具链的 CI job 才能端到端验证。
