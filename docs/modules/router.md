# 路由模块详细设计

> **文档状态**: [PROTOTYPE]  
> **版本**: v1.0.0  
> **更新日期**: 2026-08-26 (Week3 周三 — 路由)  
> **所属层**: 接入层（gateway）  
> **上游规约**: `docs/architecture/architecture-spec.md`  
> **关联 ADR**: ADR-002（模块边界）

---

## 1. 模块概述

- **定位**：网关接入层的后端定位边界——把每个请求/连接按 key（如 `player_id` / session key）路由到一致的后端实例（game 节点），实现无状态水平扩展。
- **核心职责**：
  1. **一致性哈希**：key → 后端节点的映射在节点增减时仅迁移 ~1/N 的 key，避免全量重分布（N=20 时约 5%）。
  2. **热更新**：运行时 `reload_backends` 替换后端集合，无需重启网关（配置中心 / 运维控制台下发）。
- **不在职责内（边界）**：鉴权/加密（Day1）、限流（Day2）、在线态存储（Day4 Redis）、连接迁移编排（Day5）。本模块只做"给定 key 返回哪个后端"的纯计算。

## 2. 架构约束与边界

- 是否拥有 `Player` / 连接对象：否。仅以 `std::string key` 为输入，transport-agnostic。
- 零外部依赖、纯内存、单节点；`CAMI_BUILD_MODULES=OFF/ON` 均编译（轻量 CI 友好）。
- 哈希用 **FNV-1a 64-bit**（跨平台稳定，不依赖 `std::hash` 实现差异），保证不同进程/平台同 key 同节点。
- 虚拟节点（默认 100/物理节点）使负载均衡、迁移平滑（移除 1 节点迁移 ~1/N，N=20 实测≈5%）。
- 仅文档化集成缝，**不 mutate** 早期模块：网关在需要定位后端时调用 `Router::route(key)`（缝在 Day5 连接迁移设计中统一落地）。

## 3. 对外接口（C++ 签名级）

```cpp
class Router {
    std::string route(const std::string& key) const;        // 返回后端 id，空串=无可用
    void add_backend(const std::string& id);
    void remove_backend(const std::string& id);
    void reload_backends(const std::vector<std::string>& ids);  // 热更新：替换环
    std::size_t backend_count() const;
    double migration_ratio(keys, new_backends) const;       // 验收：节点增减迁移比例
};
```

## 4. 核心数据结构

- `RouterConfig{ virtual_nodes_per_node = 100 }`。
- `HashRing`：内部 `std::map<uint64_t,std::string> ring_`（虚拟节点 hash → 物理节点 id）+ `node_vcount_` 计数；`get_node` 用 `lower_bound` 顺时针查找，越界回环到 `begin()`。

## 5. 协议引用

- 路由发生在网关决定"该连接/请求交给哪个 game 实例"时（登录后按 `player_id`，或在连接迁移 Day5 中按目标 key）。
- `route()` 返回空串表示无可用后端，网关应拒绝/排队（不在本模块处理）。

## 6. 性能预算（验收）

| 指标 | 红线 | 本模块保障 |
|------|------|-----------|
| 单次 route 判定 | < 1µs | `std::map::lower_bound` O(log V)，V=节点数×虚拟节点数（10 节点=1000 项） |
| 节点增减迁移 key | < 10% | 一致性哈希 1/N 性质：N=20 移除 1 ≈ 5%、新增 1 ≈ 4.8%（selfcheck + 单测实证，20000 key） |
| 负载均衡 | max < 2×avg | 100 虚拟节点 + FNV，20000 key 实测均衡（selfcheck 实证） |

## 7. 并发模型

- 当前为单节点内存版，`HashRing` 内部容器未加锁（原型单线程 route 路径）。
- **热更新线程安全缝**：多 acceptor / 多线程场景应改为 `std::atomic<std::shared_ptr<HashRing>>`，`reload_backends` 构建新环后原子 `store`，`route` 读取时 `load` 快照——避免读写同一环的 data race。当前原型单线程，未实现该交换（见开放问题）。

## 8. 依赖方向

```
[连接处理] ──→ [router] ──(后续)──→ [配置中心 / Redis 节点表?]（预留，非本模块必需）
                │  纯内存，零外部依赖
```

## 9. 关联 ADR / 架构章节

| 决策 | 编号 |
|------|------|
| 模块边界 WoW 模式 | ADR-002 |
| 重型依赖默认关闭 | 项目构建约定（`CAMI_BUILD_MODULES`） |
| 后期模块不 mutate 早期模块 | 工作流纪律（集成缝仅文档化） |

## 10. 开放问题 / 后续

- **热更新线程安全**：当前 `reload_backends` 直接重建环（单线程安全）；多线程应改为 atomic shared_ptr 交换（已文档化缝）。
- **后端节点来源**：运维控制台 / 配置中心 / 服务发现下发，避免硬编码（安全红线）。Day4 Redis 可承载节点表/在线态。
- **加权节点**：异构机器可给不同虚拟节点数（按权重），当前等权。
- **集成缝落地**：`Router::route(key)` 在网关定位后端处的调用点（不改 Connection 本体），Day5 连接迁移设计统一纳入。
- **CI 覆盖**：selfcheck 在轻量 CI（OFF）始终跑，迁移率/均衡为确定性实证，无需重型依赖。
