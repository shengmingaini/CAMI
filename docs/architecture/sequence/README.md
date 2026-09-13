# 关键时序（TASK-038 交付 · docs/architecture/sequence/）

## 1. Bot 单动作协议流（Login / Move / Attack / … / Reconnect）

```
Bot                ClientLink(winsock)        Gateway/MockGateway      GameNode(Scene)
 |                        |                          |                      |
 |-- Envelope(Command) -->|  [4B len][payload]  --> |  Decode(Envelope)     |
 |                        |                          |-- Command dispatch ->|
 |                        |                          |<-- Response(Envelope)-|
 |<-- [4B len][payload] --|  Decode + Validate       |                      |
 |   record tick = t1-t0  |                          |                      |
```

- 帧格式：4 字节大端长度前缀 + Envelope 编码字节（与 `mmo::net` 对称，TASK-008 §8）。
- 编解码：`mmo::protocol::FlatbufCodec`（TASK-005）；`EnvelopeValidator` 校验必填字段与版本。
- `Reconnect` 动作：关闭并重新 `Connect`，验证会话可恢复（对应 TASK-037 容灾）。

## 2. CCU 阶梯压测编排（tools/load/run_ladder.sh）

```
run_ladder.sh
  └─ for ccu in 100 500 1000 5000 10000 20000 50000:
       ├─ BotFarm.Spawn(ccu)            # sim: 内嵌 MockGateway / live: --gateway
       ├─ BotFarm.RunUntil(duration)
       ├─ 采集 metrics -> bench/load_<ccu>.txt
       └─ 断言 tick_p99 ≤ 8ms, error_rate ≤ 0.001（否则记录实际上限，不跳过）
```

## 3. Chaos 恢复时序（GameNode Crash 为例，tools/chaos）

```
chaos/gamenode_crash.sh
  ├─ 发送 SIGKILL 给目标 GameNode 进程
  ├─ 预期：Gateway 检测断连 -> Freeze 玩家会话
  ├─ ControlService 选替换节点 -> Reattach
  ├─ 断言：RTO ≤ 预期；玩家可重连；状态无丢失
  └─ 记录 RTO 与恢复断言结果到 chaos_report
```

## 4. Netsim 场景（latency / jitter / loss / bandwidth / disconnect / reconnect）

```
netsim/<scenario>.sh  ->  tc / netsh 注入损伤  ->  Bot 客户端表现校验
  预期：degraded 但不崩、可重连；5% 丢包下仍 playable；否则记录
```
