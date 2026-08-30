# engine/rpc · PERFORMANCE

> **首页红线：战斗 Tick 禁止同步 RPC。**

实测环境：Windows 11 / MSYS2 MinGW g++ 16.1.0 / gRPC 1.82.0 / Release (-O3) /
本机回环 127.0.0.1 / `bin/rpc_bench --iterations 100000` / 1 channel / 同步 stub。
机器读数见 `bench/rpc.txt`（验收脚本断言 `rpc_p99_us <= 1000`）。

| 指标 | 实测 | 目标（§22） | 结论 |
|---|---|---|---|
| rpc_p50_us | 175 | < 200 | 达标 |
| rpc_p99_us | 280 | < 1000（验收线）/ < 1ms | 达标 |
| rpc_qps | 5500 | > 20000 | **未达标（如实记录）** |
| wrapper_overhead_us | 7 | < 5 | **未达标（如实记录）** |
| timeout_overhead_us | 8 | 误差 < 10ms | 达标（8µs 量级） |

## 未达标项分析（如实记录，不粉饰）

1. **QPS 5500 << 20k**：单 channel + 同步 stub 串行调用模型下，10 万次循环
   的 QPS 由单请求 RTT 决定（~180µs → 理论上限 ~5.5k）。20k 目标需要
   并发多路（异步 stub / 多 channel / 多线程打满），属于调用方并发模型，
   不是本封装单调用路径的开销问题。p99 已满足验收线；并发 QPS 留待
   网关阶段（TASK-013+）按真实并发模型重测。
2. **wrapper_overhead 7µs（裸 stub p50 ~168µs）**：剩余开销主要是
   ClientContext/deadline 构造 + metadata 检查 + 指标记录（含一次 memcpy 键）。
   已消除每次调用 stub 重建与非幂等 UUID 生成；进一步压缩收益 <5µs，
   相对 RTT 占比 4%，暂不继续。

## 超时控制

timeout=200ms vs 服务端 sleep 1000ms 实测：客户端 ~200ms 准时返回 TIMEOUT，
误差 < 10ms（集成测试 I2 断言 elapsed < 900ms，实测 ~205ms）。
