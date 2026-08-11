# D6 后续架构待办设计（2026-08-12 产出，待拍板后实现）

> 状态：设计稿，未实现。三项待办源于 `d6-code-review.md` 第八节全面复查 + 本次 schema 契约核对。
> 原则：先测量后优化 / 零停机迁移（新增表与列，无破坏性变更）/ 每项配 Up、Down 与验证。

---

## 0. 现状问题（证据）

### P0-1：落库 SQL 与权威 schema 契约错配 ⛔
- `mysql_backing_store.cpp` 落库 SQL 引用 **`player_base(payload)`** 列：
  - `Load`: `SELECT payload FROM player_base WHERE player_id = ?`
  - `Store`: `REPLACE INTO player_base (player_id, payload) VALUES (?, ?)`
- 但权威 `docker/mysql/schema.sql` 的 `player_base` **无 `payload` 列**（只有 player_id/account_id/name/level/exp/class_id/scene_id/时间戳/version）。
- 后果：
  1. `SELECT payload` → **运行时 `Unknown column 'payload'`**（CI 编译不报错，SQL 是字符串）。
  2. `REPLACE INTO player_base (player_id, payload)` → REPLACE 是**删行重建**，会清空 account_id/name/level/exp 等结构化列；且 `account_id NOT NULL` 无默认值 → INSERT 直接失败。
- 结论：**D6 落库代理在真实 schema 上必然不可用**，是生产阻断级问题。

### P0-2：多副本并发覆盖保护未真正落地
- `VersionedStore` 是**进程内内存态**：多 Data Service 副本各自维护独立 version 表 → 跨副本 CAS 失效。
- MySQL 落库 `REPLACE` **无版本条件** → 两个副本先后落库，后者无条件覆盖（即便 version 已变）。
- 架构 §5.3「版本号防多节点并发覆盖」在**单副本**下成立，**多副本**下未落地。

### P1：缺 gRPC Health 检查
- K8s liveness/readiness probe 无口可探；Data Service 无状态自检出口。

---

## 1. 方案：新建 `player_state` 表（分片，专供序列化行存储）

### 1.1 为什么新表而不是 `player_base` 加 payload 列
- `player_base` 是**结构化基础信息**（角色创建/登录模块写：account_id/name/level/exp...）。
- Data Service 的 payload 是**序列化玩家状态 blob**（character 模块整行）。
- 若在 `player_base` 上 REPLACE 整行，会**破坏结构化列**（见 P0-1-2）——新表使两条写路径互不干扰，职责分离。

### 1.2 DDL（每分片库各建一份，经 ShardingSphere 代理一次执行即可）

```sql
-- docker/mysql/init-shard.sql + schema.sql 追加
CREATE TABLE IF NOT EXISTS `player_state` (
    `player_id`  BIGINT UNSIGNED NOT NULL                COMMENT 'PlayerID (分片键 / PK)',
    `payload`    MEDIUMBLOB     NOT NULL                COMMENT '序列化玩家行 (protobuf blob, Data Service 不解析)',
    `version`    BIGINT UNSIGNED NOT NULL DEFAULT 0     COMMENT '乐观锁版本号 (CAS 用)',
    `updated_at` DATETIME        DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`player_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='玩家序列化状态 (Data Service 落库专用)';
```

### 1.3 落库代理 SQL（`mysql_backing_store.cpp` 改目标表 + 版本语义）

| 操作 | SQL | 说明 |
|------|-----|------|
| Load | `SELECT payload, version FROM player_state WHERE player_id=? LIMIT 1` | 读 payload + 版本 |
| Store(无条件 Put) | `INSERT INTO player_state(player_id,payload,version) VALUES(?,?,1) ON DUPLICATE KEY UPDATE payload=VALUES(payload), version=version+1` | 无版本条件，版本自增 |
| Store(带版本 Cas) | `UPDATE player_state SET payload=?, version=version+1 WHERE player_id=? AND version=?` | **affected_rows==1 成功 / 0 冲突** |
| Delete | `DELETE FROM player_state WHERE player_id=?` | 删除语义（沿用空 value 约定） |

- `BackingStore::Store` 签名需扩展为带版本：`Store(key, value, expected_version)`（`expected_version<0` 表示无条件），或新增 `CasStore`。**推荐后者**：`Store` 保持无条件，新增 `bool CasStore(key, value, expected_version)`，Kafka consumer 落库时用 CasStore。
- player_id 绑定沿用 `MYSQL_TYPE_LONGLONG + is_unsigned`（64 位，防截断，已有实践）。

### 1.4 迁移 / 回滚
- **Up**：新表 DDL（`CREATE TABLE IF NOT EXISTS`，8 分库各建一份）——零停机，不动既有表。
- **Down**：`DROP TABLE IF EXISTS player_state;` + 落库 SQL 回退 `player_base`（当前版本）。
- **验证**：docker compose 起 SS + 8 分库 → 直插/回读 player_state → 确认 Load/Store/Cas/Delete 四路径；`build-modules-on` CI 编译绿。

---

## 2. 多副本 CAS 下沉 MySQL（真实并发保护）

### 2.1 设计目标
以 **MySQL `player_state.version` 为权威版本源**，多 Data Service 副本的并发写经 DB 层版本条件裁决，冲突方不覆盖。

### 2.2 方案 A（最小，推荐先做）：仅落库端 CAS 下沉
- 链路不变：GameNode → DataClient(gRPC) → CacheProxy.WriteBack(dirty) → Kafka → KafkaSinkWorker → MySQL。
- 改动：
  1. Kafka 消息已带 `version`（现有 `FlushMessage.version`）✅
  2. consumer 落库从「`version.Cas`(内存) + `Store(REPLACE)`」改为「`CasStore(key, value, msg.version)`（MySQL `UPDATE ... WHERE version=?` 判 affected_rows）」。
  3. `affected_rows==0`（版本冲突）→ 消息进 DLQ（保留冲突痕迹，人工/重放裁决），不覆盖。
  4. `VersionedStore` 本地版本保留用于 Data Service 读写路径的快速裁决（近似），但**最终裁决在 DB**。
- 验证：双 Data Service 副本并发写同一 player_id → 后写者 version 不匹配进 DLQ，DB 保持先写者数据。

### 2.3 方案 B（完整，后续迭代）：读写路径均以 DB 版本为权威
- `Get` 回源时带出 `player_state.version`，写回本地版本缓存（读路径多一次 DB 查询，读延迟↑）。
- `Cas` 直接执行 DB 层 CAS（`UPDATE ... WHERE version=?`），成功才写缓存。
- 本地 `VersionedStore` 退化为纯缓存（可加 TTL/对账）。
- 代价：读放大；收益：跨副本强一致裁决。**建议方案 A 落地并实测后再评估是否需要 B**。

---

## 3. gRPC Health RPC

### 3.1 推荐：gRPC 内置 Health 服务（零 proto 改动）
```cpp
// data_service_main.cpp 启动前加一行：
grpc::EnableDefaultHealthCheckService(true);
```
- 自动注册 `grpc.health.v1.Health/Check`，K8s probe 用官方 `grpc-health-probe -addr=:50051 -service=`（镜像含该二进制或宿主安装）。
- 依赖：`grpcpp/health_check_service_interface.h`（gRPC C++ 自带，无需 vcpkg 新增）。

### 3.2 可选增强：自定义 `Ping` RPC（运维排查用）
- `proto/data_service.proto` 加 `rpc Ping(PingReq) returns (PingResp)`，返回进程版本/存活时间。
- 二选一即可；**推荐先做 3.1（K8s 必需），Ping 按需加**。

### 3.3 验证
- 本地起 Data Service → `grpc-health-probe -addr=127.0.0.1:50051` 返回 `SERVING`。

---

## 4. 实施顺序与工作量（建议）

| 步 | 内容 | 文件 | 量级 |
|----|------|------|------|
| 1 | `player_state` DDL（schema.sql + init-shard.sql） | 2 个 SQL | 小 |
| 2 | `BackingStore` 增 `CasStore` + MySQL 实现（四路径 SQL） | mysql_backing_store.{h,cpp} | 中 |
| 3 | Kafka consumer 落库改 `CasStore`（冲突进 DLQ） | data_service_main.cpp / kafka_flush.cpp | 小 |
| 4 | `EnableDefaultHealthCheckService(true)` | data_service_main.cpp | 极小 |
| 5 | docker 起环境验证四路径 + 双副本冲突用例 | docker + 脚本 | 中 |

> 步 1-4 全在 MODULES 门控 / 新增表内，OFF 构建不受影响；步 5 需 Docker（沙箱不可达，留本机/CI）。
> **拍板项**：方案 A（最小）还是直接上 B？`CasStore` 接口命名可调整。

---

## 5. 风险与红线
- `player_state` 与 `player_base` 双写一致性：角色创建时两表都写（跨分片无事务）——靠**顺序 + 幂等**（先 player_base 后 player_state；player_state 幂等 UPSERT），不做分布式事务。
- 方案 A 下本地 version 可能短暂落后 DB（30s 落库窗口内）——读写路径仍可能放行一次覆盖，但 DB 层最终裁决拦截落库冲突；严格一致场景（交易/货币）应走 WriteThrough + 后续方案 B。
- 不改动 `player_base` 现有结构化语义；不动 ShardingSphere 配置（player_state 同分片键复用现有路由）。
- 落库 SQL 变更在非生产（docker 环境）验证后才上生产，绝不未经确认执行 DROP/破坏性变更。
