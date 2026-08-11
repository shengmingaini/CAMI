# 集成层（GatewayPipeline）设计文档

> **模块**: gateway/integration | **target**: `cami_gateway_integration` (STATIC)
> **所属层**: 接入层（gateway）| **关联**: ratelimit(D2) / security(D1) / router(D3) / redis(D4)
> **上游规约**: `docs/design/connection-migration.md` §3（四模块集成缝总表）

---

## 1. 概述

`GatewayPipeline` 把 Week3 前四天交付的四个网关模块的**集成缝**组合成一个可单测的策略层，
将"鉴权 → 限流 → 选后端 → 在线态"串成网关接入路径上的统一策略。

**核心纪律**：不修改 `connection.cpp` / `connection_manager.cpp` 任何逻辑；仅通过
`ConnectionManager` 既有的 `set_on_accept` 挂钩接入。四道缝对应方法均为纯函数式策略，
可脱离真实 socket 单测（见 `tests/unit/gateway_pipeline_test.cpp`）。

## 2. 接口

```cpp
GatewayPipeline(const RateLimiterConfig& rl_cfg,
                security::TokenAuth auth,
                router::Router router,
                std::shared_ptr<OnlineStateStore> store,
                ClockFn now);

// 缝① 限流：新连接接入，按对端 IP 限流
ratelimit::RateLimitResult on_accept(const std::string& peer_ip);
void ban(const std::string& ip, int64_t ttl_ms);   // 运行时封禁
void allow(const std::string& ip);                  // 运行时放行

// 缝② 鉴权：首包 token 校验，成功填充 player_id
bool authenticate(const std::string& token, uint64_t& player_id_out);

// 缝③ 选后端：player_id -> game 后端
std::string route_backend(uint64_t player_id) const;

// 缝④ 在线态：会话建立/下线
void on_session_established(uint64_t player_id, const std::string& backend);
void on_session_closed(uint64_t player_id);
std::shared_ptr<OnlineStateStore> get_store() const;

// 一键接线：仅设置 ConnectionManager 既有 on_accept 挂钩（deny 即 close）
void install(connection::ConnectionManager& mgr);
```

## 3. 四道缝与调用方

| 缝 | 模块 | 调用点 | 由谁调用 |
|----|------|--------|----------|
| ① 限流 | ratelimit | `on_accept(peer_ip)` | `install()` 注入的 `on_accept` 钩子（deny→`close()`） |
| ② 鉴权 | security | `authenticate(token, pid)` | 上层首包解析出 token 后调用，失败断连 |
| ③ 选后端 | router | `route_backend(pid)` | 登录/迁移完成后调用 |
| ④ 在线态 | redis | `on_session_established/closed` | 会话建立/下线/迁移切换时调用 |

> 缝②/③/④ 由上层登录/迁移流程显式调用——`Connection` 不携带 `player_id`，
> 会话↔连接 映射属于应用层，不在集成层内（避免为塞入钩子而扭曲设计）。

## 4. 数据结构 / 依赖

- 组合持有：`RateLimiter`（限流）、`TokenAuth`（鉴权）、`Router`（路由）、
  `shared_ptr<OnlineStateStore>`（在线态，默认 `InMemoryState`，`MODULES=ON` 下可注入
  `RedisClusterState`）。
- 链接：`cami_gateway_ratelimit` / `security` / `router` / `redis` / `connection`。
- 依赖下层：无新增（transport-agnostic）。

## 5. 并发

- `GatewayPipeline` 本身不持有锁；其组合的子模块各自有无锁/单线程假设（见各模块文档）。
- `install()` 在 `ConnectionManager::start()` 之前调用一次即可；`on_accept` 在多 io_context
  线程并发触发，需各子模块自身线程安全（ratelimit/router 当前为单网关进程内无锁设计，
  多进程水平扩展时无共享状态）。

## 6. 测试与验收

- 确定性 selfcheck（`gateway_pipeline_selfcheck`，返回 bool，无 GTest/socket）：黑/白名单、
  合法/非法 token、路由确定性、在线态建/销。
- GTest `cami_integration_test`：6 例边界（黑名单 deny / 白名单 allow / 普通 allow / 鉴权
  合法+非法 / 路由确定性 / 在线态往返）。
- 验收：本地 `ctest` 集成模块全绿；`MODULES=OFF` 轻量 CI 始终可编译+selfcheck 通过。

## 7. 开放问题

- 真实 Redis 后端（`RedisClusterState`）运行期注入与迁移控制器的落点查询，需在
  `MODULES=ON` + 运行 Redis Cluster 的 CI job 端到端验证（见周报风险②）。
- 800ms 迁移预算实测需真实多网关压测（环境不可达，见周报风险④）。
