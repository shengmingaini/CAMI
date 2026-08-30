# protocol · 性能报告（PERFORMANCE）

> 全部为 `scripts/verify/task-005.sh` 同口径实测：MSYS2 MinGW-w64 g++ 16.1.0，
> Release `-O3`，`protocol_bench --iterations 1000000`（1e6 次小消息，payload 128B）。
> 机器：Windows + MinGW（数据为本机实测，非理论值）。

## 实测结果（2026-08-30，Release）

| 指标 | 实测值 | 阈值（任务书 §22 / §24） | 余量 |
|---|---|---|---|
| `pb_encode_ns` | **285.1 ns** | < 2000 ns | 85.7% |
| `pb_decode_ns` | **328.4 ns** | < 2000 ns | 83.6% |
| `fbs_encode_ns` | **105.1 ns** | < 300 ns | 65.0% |
| `fbs_decode_ns` | **41.0 ns** | ≤ 300 ns | 86.3% |
| `fbs_decode_allocs` | **0** | = 0（零拷贝） | 达标 |
| `alloc_per_op` | 1.000 | —（Protobuf 解码必然构造消息，1 次/操作） | — |

Debug 参考值（非验收口径）：`fbs_decode_ns=1024`（Debug 无优化，仅供对照）。

## 口径说明

- `fbs_decode_allocs`：全局 `operator new/delete` 重载计数（含 array 变体），
  计数窗口为 1e6 次 `FlatbufCodec::Decode` 循环。为 0 证明解码全程零堆分配：
  `Verifier` 校验 + `GetRoot` 根偏移读取 + `EnvelopeView` 全借用输入缓冲。
- `alloc_per_op`：同窗口下 `ProtobufCodec::Decode` 的平均分配次数。
  Protobuf 解码必须构造 `MessageEnvelope` 对象（1 次/new），此为库语义，非本模块开销。
- 反优化措施：每次解码结果的关键指针 / 长度写入 `volatile` sink，防 `-O3` 消除循环体。
- 小消息定义（§22）：序列化后 < 256B；本基准 payload 128B + 信封字段。

## 结论

两条编解码链路全部达到任务书 §22 性能预期；FlatBuffers 解码零拷贝达标
（41 ns，比 Protobuf 解码快约 8 倍，符合"高频热路径用 FlatBuffers"的选型依据）。
