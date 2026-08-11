# 账号 / 角色分离设计与单账号角色上限（n-cap）

> 文档状态: [PRODUCTION] 设计稿 — Week 4 Day 1 补充（2026-08-11 确认）
> 关联: `docker/mysql/schema.sql` §一、 `docker/mysql/init-global.sql` `account` 表
> 验收: 账号与角色号分离；单账号最多 n 角色（当前 n=5，后续可扩 8-10）；上限由配置驱动、免 DDL 变更。

## 1. 为什么分离

账号（account）先于角色（player）存在，**无法以 `player_id` 路由**，因此落在全局库 `cami_global`，不随玩家分片。角色按 `player_id` 散列到 8 个分片库。两者通过逻辑外键关联：

- `player_base.account_id` → 引用 `cami_global.account.account_id`
- `account_character(account_id, player_id)` → 全局映射，登录时 O(1) 列举某账号全部角色，避免广播 8 分库
- `player_name_reservation(name)` → 全局占名，收口"角色名跨分片唯一"

## 2. 单账号角色上限（n-cap）

| 项 | 取值 | 说明 |
|----|------|------|
| 当前上限 `n` | **5** | 业务配置（配置中心 / Lua），非硬编码 |
| 可扩展上限 | **8~10** | 后续改配置即可，无需 DDL 迁移 |
| 计数列 | `account.character_count` | 缓存计数，建/删角色时维护 |
| DB 层兜底约束 | `CHECK (character_count BETWEEN 0 AND 100)` | 仅防脏数据，宽松上限，不承载业务 n |

## 3. 强制机制（核心）

`account` 是全局库中的**单行热点**，无需乐观锁版本即可原子拦截——用一条条件更新占名额：

```sql
-- Data Service 创角事务第一步: 占名额 (n 取自配置)
UPDATE cami_global.account
   SET character_count = character_count + 1,
       version         = version + 1          -- 账号级并发仍建议带版本
 WHERE account_id = ? AND character_count < ?; -- ? = 当前 n (5)
-- affected_rows == 1 -> 名额占用成功, 继续建角色
-- affected_rows == 0 -> 已达上限, 直接拒绝, 不落角色
```

该语句在 InnoDB 行锁下原子执行，**多个 GameNode 并发为同一账号创角也不会突破上限**（行锁串行化 + `WHERE character_count < n` 条件判定）。这是比应用层先 `SELECT COUNT` 再 `INSERT` 更安全的写法（后者存在竞态）。

### 跨库事务顺序（先占名额，后落角色）

1. 全局库事务: 占名额（`UPDATE ... WHERE character_count < n`，affected=1）。
2. 分片库事务: 插 `player_base` + 全局库插 `account_character` + `player_name_reservation`（占名）。
3. 任一步失败: 回滚名额（`UPDATE account SET character_count = character_count - 1 WHERE account_id = ?`）。
   - 严格一致性可用 XA/Seata 2PC；容忍短暂不一致可用"名额先占、角色异步确认、超时回退"的最终一致方案。
4. 删角色: 反向 `character_count - 1`（带 `> 0` 保护），并清 `account_character` / `player_name_reservation`（软删 `deleted_at`）。

## 4. 与分片路由的配合

- 账号相关操作（登录、列角色、占名额）全部走 `cami_global`，**不经 ShardingSphere 分片路由**。
- 角色相关操作（读 `player_base`、背包、货币、邮件）以 `player_id` 路由到对应分片。
- 创角流程的入口在全局库，成功后才产生 `player_id` 并写入分片——因此不存在"先有角色后补账号"的路径。

## 5. 验收对照

| 验收项 | 结果 |
|--------|------|
| 账号与角色号分离 | ✅ `account` 在 `cami_global`，`player_*` 在分片库 |
| 单账号 ≤ n 角色（n=5，可扩 8-10） | ✅ 原子条件更新强制 + 配置驱动免 DDL |
| 跨分片唯一性收口 | ✅ `player_name_reservation` |
| 登录列举免广播 | ✅ `account_character` |
| 3NF | ✅ `character_count` 为账户自身属性，非派生；`account_id`/`player_id` 为引用 |

## 6. 实库验证

见 `docker/mysql/verify-ddl.sql`：校验表/列/CHECK，并**实际演示第 6 次创角被拒**（`ROW_COUNT()=0`）。运行方式见周报/运行手册（沙箱 Docker 或本地 MySQL 均支持）。
