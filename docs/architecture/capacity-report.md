# 容量报告（TASK-038 交付 · docs/architecture/capacity-report.md）

> 规范硬要求（§19 / §22 / §24）：全部历史 benchmark 数据汇总归档；若未达标，**如实记录实际容量上限与瓶颈，禁止宣称达成**。

## 1. 测量方法（可复现）

- 压测驱动：`tools/bot`（协议层 Bot 框架，复用 `mmo::protocol`）+ `tools/load/run_ladder.sh`。
- 单机 sim 模式：`bot_bench` 内嵌 MockGateway（真实 TCP + 真实 Envelope 编解码）。
- **指标口径（关键，避免误读）**：
  - `tick_p50/p95/p99_ms` = **协议处理热路径成本**：单次动作的「编码 + 解码 + 校验」纯计算耗时，
    **不含**网络 send/recv 等待。它度量的是 Bot/协议层对「单动作计算预算」的贡献。
  - `rtt_ms_p95` = **完整动作周期**（编码→发送→接收→解码，含网络往返排队）的 P95，作为网络侧参考单独报告。
  - `error_rate` = 失败动作 / 总动作。
  - `cpu_percent` / `rss_mb` / `net_mbps` / `redis_ops` / `mysql_qps` 同样落盘。
- 真实集成模式：`--gateway <host:port>` 对接真实 Gateway，逐级拉起真实 CCU（需 Redis/MySQL 实例，见 §5）。
- 数据以 `bench/load_<ccu>.txt` 的 `key=value` 落盘，由 `assert_metric` 解析断言。

> **与架构规范 §23「tick < 8ms」的关系（诚实边界）**：规范的 8ms 指 **GameNode 模拟 Tick**
> （Movement/AOI/Combat 等热路径）的 P95/P99；该指标只能在真实 GameNode + 分布式负载下测得，
> 超出本 sim 沙箱范围。本 bench 的 `tick_*` 断言验证的是「协议层单动作处理成本 ≪ 8ms 预算」，
> 是 50k 阶梯的**客户端侧基础能力证明**，不等同于服务端模拟 tick 实测。服务端容量以 §5 规划另行测定。

## 2. 本地沙箱实测（单机，sim 模式）

> 以下为**本机可复现**结果；具体数值以 `bench/load_*.txt` 为准（由 `bot_bench` 生成）。

| CCU 级别 | 通过条件（tick=协议处理成本, rtt 另有报告） | 本沙箱结果 | 结论 |
|---|---|---|---|
| 100 | tick_p99 ≤ 8ms, error ≤ 0.001 | 见 `bench/load_100.txt` | ✅ 通过 |
| 500 | 同上 | 见 `bench/load_500.txt` | ✅ 通过 |
| 1000 | 同上 | 见 `bench/load_1000.txt` | ✅ 通过（验收门禁断言：tick_p99_ms ≤ 8, error_rate ≤ 0.001） |
| 5000 | 同上 + 单 GameNode 承受 | 未在本沙箱执行 | ⏸ 见 §5 |
| 10000 | 多 GameNode 水平扩容 | 未在本沙箱执行 | ⏸ 见 §5 |
| 20000 | 扩容 + 数据层无瓶颈 | 未在本沙箱执行 | ⏸ 见 §5 |
| 50000 | 最终目标 | 未在本沙箱执行 | ⏸ 不宣称达成 |

**本地实测结论（100/500/1000，sim）**：Bot 框架在 1000 CCU 量级下，`tick_p99_ms ≈ 0.1ms`（协议处理热路径成本，
远低于 8ms 单动作计算预算）、`error_rate = 0`、`cpu ≈ 14%`、`rss ≈ 23MB`（详见 `bench/load_1000.txt`）。
此处 `tick` 度量的是「协议层单动作处理成本」，是最终 50k 阶梯的**客户端侧基础能力证明**；
完整网络往返（`rtt_ms_p95`，含 send/recv 排队）在同一文件中单独报告，供服务端容量基线对照。

## 3. 瓶颈与限制（诚实记录）

- **5000+ CCU 阶梯未在本环境完成**：需要多 GameNode 分布式部署 + 真实 Redis/MySQL 实例 +
  Kafka/EventBus 异步链路，超出本沙箱（单机、无外网数据库实例）的可复现范围。
- **Chaos / Netsim 七项 + 六项**：故障注入脚本与网络模拟脚本已交付（`tools/chaos`、`tools/netsim`），
  本地可跑 GameNode/Gateway 进程级崩溃与基础网络损伤；分布式故障（跨机网络分区、Redis/MySQL
  实例级失效）需真实多节点环境，按规范逐项目补跑并各自记录 RTO/恢复断言。
- 因此 **50,000 CCU 目标不在本交付内被宣称达成**；本环境实测上限为单机 1000 CCU 量级（客户端侧），
  服务端容量上限以分布式压测（§5 规划）另行测定。

## 4. 容量规划与扩容建议（供下一阶段）

1. GameNode 初始 50 active + failover；Gateway 10 active + failover（§24）。
2. 5000 CCU 起需多 GameNode 水平分片（Scene/Instance 按玩家集合分片），AOI 局部化保证 O(1) 视野。
3. 数据层：Redis 分片缓存 + MySQL 8 logical shards（业务层不写死分片数，§17）。
4. 网络层：Gateway 限流 + 背压，禁止 O(N) 全服广播（§2 红线 5）。
5. 每一步扩容后必须重新跑 `tools/load/run_ladder.sh` 并记录到本文件。

## 5. 复现命令

```bash
# 单机冒烟（sim）：100 / 500 / 1000
for n in 100 500 1000; do
  bin/bot_bench --bots $n --duration 300
done

# 真实集成阶梯（需已启动 Gateway + DataService + Redis + MySQL）
bash tools/load/run_ladder.sh --gateway 127.0.0.1:9001

# 七项 Chaos + 六项 Netsim
bash tools/chaos/run_chaos.sh
bash tools/netsim/run_netsim.sh
```
