# Week4 D2 — ShardingSphere 8 分库 + 读写分离验证报告

> 验收目标：**跨分片查询路由正确**（分片键统一 `player_id`，8 分库；读写分离 8 主 8 从）
> 环境：Docker 沙箱，MySQL 8.0.46，ShardingSphere-Proxy 5.5.0
> 实测结论：**全部通过** ✅

---

## 1. 拓扑

```
                          ┌─────────────────────────────────────┐
   应用 / Data Service ──▶│  ShardingSphere-Proxy  (3309→3307)   │
                          │  databaseName: cami_db               │
                          │  login: root / root                  │
                          └───────────────┬─────────────────────┘
                                          │ 分片 player_id % 8
              ┌───────────────────────────┼───────────────────────────┐
              │ (每分片 = 1 主 + 1 从, GTID 复制)                        │
        ds_0: mysql-shard-0(m) / mysql-shard-0-s(s)
        ds_1: mysql-shard-1(m) / mysql-shard-1-s(s)
        ...
        ds_7: mysql-shard-7(m) / mysql-shard-7-s(s)
              └───────────────────────────────────────────────────────┘
   全局库 cami_global (account / account_character / player_name_reservation / auction_* / guild_*) 独立, 不走 player_id 分片
```

- 分片键：`player_id`（`player_id % 8` → `ds_0..ds_7`）
- 读写分离：写 → `*_m`（主），读 → `*_s`（从，GTID `AUTO_POSITION` 复制）
- 逻辑库 `cami_db` 暴露 8 张玩家表；物理库 `cami_shard_0..7`

## 2. 配置落点

| 文件 | 内容 |
|------|------|
| `docker/shardingsphere/config-cami_db.yaml` | 8 分片 + 读写分离规则（**已修正 SS 5.5 语法**） |
| `docker/shardingsphere/server.yaml` | 代理登录 `root/root`、前端 MySQL 协议 |
| `docker/mysql/repl-master.sql` | 主库建 `repl` 复制账号 + `GRANT REPLICATION SLAVE` |
| `docker/mysql/repl-slave.sh` | 从库 `CHANGE MASTER TO ... MASTER_AUTO_POSITION=1` + `START REPLICA` |
| `docker/docker-compose.yml` | 8 主 + 8 从 + 全局 + SS 代理 + 17 卷 |

## 3. ⚠️ 关键坑（已修复，务必记牢）

**ShardingSphere 5.2+ 改了 `READWRITE_SPLITTING` 的 YAML 语法**，旧的 `type: Static` + `props:` 包裹层已删除，否则代理启动直接崩：

```text
YAMLException: Unable to find property 'type' on class:
  org.apache.shardingsphere.readwritesplitting.yaml.config.rule
  .YamlReadwriteSplittingDataSourceRuleConfiguration
```

✅ 正确写法（SS 5.5.0）：

```yaml
- !READWRITE_SPLITTING
  dataSources:
    ds_0:
      writeDataSourceName: ds_0_m
      readDataSourceNames:
        - ds_0_s
```

另一个坑：**改了 `config-cami_db.yaml` 后必须 `docker compose up -d --force-recreate shardingsphere-proxy`**。`up -d` 只比对挂载声明、不比对挂载文件内容，旧容器会一直跑着内存里的旧 `%2` 算法，表现为"配置改了却不生效"。

从库启动也不能带 `--super_read_only=ON`：复制控制语句（`CHANGE MASTER`/`START REPLICA`）会被拦截导致容器中止。应裸启动，复制起来后再动态 `SET GLOBAL read_only=ON`。

## 4. 验证结果（实跑证据）

### 4.1 单分片路由 `player_id % 8`
| player_id | 期望分片 | 实际落点 | 结果 |
|-----------|----------|----------|------|
| 1 | 1 | 1 | ✅ |
| 2 | 2 | 2 | ✅ |
| 3 | 3 | 3 | ✅ |
| 4 | 4 | 4 | ✅ |
| 5 | 5 | 5 | ✅ |
| 6 | 6 | 6 | ✅ |
| 7 | 7 | 7 | ✅ |
| 8 | 0 | 0 | ✅ |
| 100 | 4 | 4 | ✅ |
| 255 | 7 | 7 | ✅ |
| 1000 | 0 | 0 | ✅ |
| 8888 | 0 | 0 | ✅ |

每行的 `COUNT(*)` 在**恰好一个**分片 = 1，其余分片 = 0（无广播写）。

### 4.2 跨分片查询
| 查询 | 结果 | 说明 |
|------|------|------|
| `SELECT COUNT(*) FROM player_base` | 13 | 跨 8 分片聚合合并 ✅ |
| `WHERE player_id IN (1,8,100)` | 1,8,100 | 多分片定向路由 ✅ |
| `WHERE class_id=1`（无分片键） | 13 | 广播全 8 分片后合并 ✅ |
| `SHOW TABLES` | 8 张 `player_*` 逻辑表 | 逻辑表暴露正确 ✅ |

### 4.3 读写分离（主从差异法）
1. `STOP REPLICA` on `mysql-shard-1-s`（停复制）
2. 经 SS 写入 `player_id=9` → 主库 `shard-1` 有（count=1），从库 `shard-1-s` 无（count=0）
3. 经 SS `SELECT player_id=9` → **返回空** ⇒ 读请求落到了从库 ✅
4. `START REPLICA` 后再次经 SS 读 → 返回 `9, n9` ⇒ 从库追平、读路径正常 ✅

结论：**写 → 主库、读 → 从库**，读写分离生效。

## 5. 复现命令

```bash
cd F:/AI/workbuddy/CAMI
# 起全部 17 个 MySQL + SS（首次拉镜像，约 1-2 min）
docker compose -f docker/docker-compose.yml up -d \
  mysql-global mysql-shard-0 mysql-shard-0-s mysql-shard-1 mysql-shard-1-s \
  mysql-shard-2 mysql-shard-2-s mysql-shard-3 mysql-shard-3-s \
  mysql-shard-4 mysql-shard-4-s mysql-shard-5 mysql-shard-5-s \
  mysql-shard-6 mysql-shard-6-s mysql-shard-7 mysql-shard-7-s \
  shardingsphere-proxy
# 改了 SS 配置后必须重建代理
docker compose -f docker/docker-compose.yml up -d --force-recreate shardingsphere-proxy
# 连代理（容器内的 mysql 客户端，登录 root/root，库 cami_db）
docker compose -f docker/docker-compose.yml exec -T mysql-shard-0 \
  mysql -uroot -proot -h shardingsphere-proxy -P 3307 cami_db
```

连接信息：SS 代理 `shardingsphere-proxy:3307`（映射宿主机 `3309`），登录 `root/root`；后端 MySQL `root/cami_dev_2026`。

## 6. 验收对照

| 验收项 | 结果 |
|--------|------|
| 分片键统一 `player_id` | ✅ 8 张玩家表均以 `player_id` 为分片键 |
| 8 分库路由正确 | ✅ `player_id % 8` 逐行验证通过 |
| 跨分片查询路由正确 | ✅ 聚合 / 定向 IN / 广播合并均正确 |
| 读写分离 | ✅ 写主读从，主从差异法实测通过 |
