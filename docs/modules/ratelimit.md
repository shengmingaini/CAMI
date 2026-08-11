# 限流防攻击模块详细设计

> **文档状态**: [PROTOTYPE]  
> **版本**: v1.0.0  
> **更新日期**: 2026-08-25 (Week3 周二 — 限流防攻击)  
> **所属层**: 接入层（gateway）  
> **上游规约**: `docs/architecture/architecture-spec.md`  
> **关联 ADR**: ADR-002（模块边界）

---

## 1. 模块概述

- **定位**：网关接入层的防攻击边界——在连接建立与消息上行阶段，按源 IP 实施限流，抵御恶意洪峰（DDoS / 连接耗尽 / 爆破），保障正常玩家不受影响。
- **核心职责**：
  1. 每源 IP **令牌桶**：平滑限流总请求/字节速率。
  2. 每源 IP **连接频率限制**：固定窗口内限制新建连接数，防连接耗尽。
  3. **IP 黑白名单**：白名单永久直通；黑名单支持 TTL 过期（TTL≤0 表示永久封禁）。
- **不在职责内（边界）**：鉴权/加密（Day1 security）、路由（Day3 router）、在线态存储后端（Day4 Redis）。本模块只做"是否放行该 IP 的连接/请求"的判定，不持有连接对象。

## 2. 架构约束与边界

- 是否拥有连接对象 / `Player`：否。仅以 `std::string ip` 为键，transport-agnostic。
- 零外部依赖、纯内存、单节点；`CAMI_BUILD_MODULES=OFF/ON` 均编译（轻量 CI 友好）。
- 时钟可注入（`ClockFn = std::function<int64_t()>`）；selfcheck 与单测注入假时钟做确定性验证，生产默认走 `steady_clock`。
- 仅文档化集成缝，**不 mutate** Monday 的 `ConnectionManager`：连接层在 `accept` / `on_data` 时调用 `RateLimiter::check(ip)`，判定拒绝则主动断连（缝在 Day5 连接迁移设计中统一落地）。

## 3. 对外接口（C++ 签名级）

```cpp
// 令牌桶（单 IP）
class TokenBucket {
    bool allow(int64_t tokens = 1);  // 有余量消费并返回 true
    double tokens() const;
};

// 连接频率限制（固定窗口，单 IP）
class ConnFreqLimiter {
    bool allow();  // 窗口内未超上限返回 true，否则 false（窗口滑动后自动重置）
};

// IP 黑白名单（TTL 过期；ttl_ms<=0 永久）
class IpList {
    void ban(const std::string& ip, int64_t ttl_ms);
    void unban(const std::string& ip);
    void allow(const std::string& ip);            // 加入白名单（永久）
    bool is_whitelisted(const std::string&) const;
    bool is_blacklisted(const std::string&) const; // 过期返回 false
    void prune();                                  // 清理过期黑名单项
};

// 限流门面
class RateLimiter {
    RateLimitResult check(const std::string& ip);  // 优先级: 白 > 黑 > 桶 > 频率
    void ban(const std::string& ip, int64_t ttl_ms);
    void allow(const std::string& ip);
};
```

## 4. 核心数据结构

- `RateLimitResult`：`kAllow=0 / kAllowWhitelist=1 / kDenyBlacklist=2 / kDenyTokenBucket=3 / kDenyConnFreq=4`。
- `TokenBucketConfig{ capacity=100, refill_per_sec=50 }`、`ConnFreqConfig{ max_per_window=20, window_sec=1 }`、`RateLimiterConfig{ token_bucket, conn_freq }`。
- `IpList::blacklist_`：`unordered_map<string, int64_t>`（值=过期毫秒，0=永久）；`whitelist_`：`unordered_set<string>`。

## 5. 协议引用

- 判定发生在连接层 accept / 首包 on_data 之前（或在每包 on_data 处对上行做令牌桶计数）。
- `check()` 返回 deny 类结果时，连接层应直接拒绝/断连；白名单用于运维/内部网段快速放行。

## 6. 性能预算（验收）

| 指标 | 红线 | 本模块保障 |
|------|------|-----------|
| 单 IP check 判定 | < 0.5µs | 纯内存哈希查找 + 整数运算（无锁、无系统调用） |
| 恶意洪峰下正常玩家 | 不受影响 | 限流按 IP 隔离；白名单路径 O(1) 直通 |

> 验收方法：注入假时钟的 selfcheck（CI 始终跑）+ `cami_ratelimit_test`（GTest 边界）。洪峰/正常隔离通过压测（周四 stress 套件）观察正常玩家 RTT 不受污染。

## 7. 并发模型

- 当前为单节点内存版，`buckets_` / `freqs_` / `IpList` 内部容器未加锁（原型单线程判定路径）。
- 多 acceptor 场景（Linux `SO_REUSEPORT`）下，跨 acceptor 共享 `RateLimiter` 需外部加锁或按 acceptor 分片；**当前原型不做跨线程共享**，集成缝在 Day5 连接迁移 / 周四 Redis 在线态设计中明确。

## 8. 依赖方向

```
[连接处理] ──→ [ratelimit] ──(后续)──→ [Redis 在线名单?]（预留，非本模块必需）
                │  纯内存，零外部依赖
```

## 9. 关联 ADR / 架构章节

| 决策 | 编号 |
|------|------|
| 模块边界 WoW 模式 | ADR-002 |
| 重型依赖默认关闭 | 项目构建约定（`CAMI_BUILD_MODULES`） |
| 后期模块不 mutate 早期模块 | 工作流纪律（集成缝仅文档化） |

## 10. 开放问题 / 后续

- **分布式限流**：当前单节点内存，跨网关实例需共享状态（Redis 计数器 / 一致性哈希分片，Day3 router + Day4 Redis）。
- **白名单来源**：运维控制台 / 配置中心下发，避免硬编码（安全红线）。
- **集成缝落地**：`RateLimiter::check` 在 `ConnectionManager::on_accept` / `on_data` 的调用点（不改 Connection 本体），Day5 连接迁移设计统一纳入。
- **阈值调参**：`TokenBucketConfig` / `ConnFreqConfig` 默认值需结合压测（周四 stress 套件）校准，写入 `balance.json` 类似配置（若纳入经济/数值平衡校验，则同步 verifier）。
