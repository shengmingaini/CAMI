# protocol · 测试说明（TEST）

> 测试可执行：`protocol_test`（§16 单元 / §17 集成 / §19 Failure 合一），
> 注册为 ctest `Protocol.Suite`，工作目录 = 仓库根。
> 运行：`ctest --test-dir build/Release -R Protocol` 或直接 `bin/protocol_test`。

## §16 单元测试

| 用例 | 覆盖 |
|---|---|
| `T01_PbRoundTripRandom1000` | Protobuf 1000 次随机数据全字段往返一致（含 10 字段 + transaction_id / idempotency_key） |
| `T02_FbsRoundTripRandom1000` | FlatBuffers 1000 次随机数据往返一致 |
| `T03_FbsZeroCopyViewPointsIntoInput` | 零拷贝断言：解码后 view 的 source / trace_id / payload / transaction_id / idempotency_key 指针全部落在输入缓冲内 |
| `T04_ValidatorRequiredFields` | 空 source / 未知类型 / Command 空 payload → `INVALID_ARGUMENT` |
| `T05_VersionConflictNoSilentDowngrade` | version 相同放行；更高 / 更低 → `VERSION_CONFLICT`（不静默降级） |
| `T06_MessageTypeRoundTripPbAndFbs` | 5 种消息类型 × 2 条链路，类型识别往返正确 |
| `T07_ErrorCodeToStringDistinct` | 错误码 `ToString` 非空且可区分 |

## §17 集成测试

| 用例 | 覆盖 |
|---|---|
| `T08_CrossProcessFileRoundTripPb` | 跨进程模拟：A 编码 → 文件落盘 → B 重新读取解码，结果一致（Protobuf 链路） |
| `T09_CrossProcessFileRoundTripFbs` | 同上（FlatBuffers 链路） |
| `T10_EconomyIdempotencyPassthroughPbAndFbs` | 经济类消息 `transaction_id` + `idempotency_key` 双链路透传不丢 |

## §19 Failure 测试

| 用例 | 覆盖 |
|---|---|
| `T11_HeartbeatAllowsEmptyPayload` | 心跳消息允许空 payload（对照：C/Q/E 空载荷被拒） |
| `T12_TruncationFuzz1000NoCrash` | 对两条链路各 1000 次随机截断解码——返回错误或成功，绝不崩溃 / 越界 |
| `T13_OversizePayloadBlocked` | > 1 MiB payload（1 MiB + 16B）被 `kMaxPayloadBytes` 拦截 → `INVALID_ARGUMENT` |

## Benchmark

`protocol_bench --iterations N`：输出 `pb_encode_ns / pb_decode_ns / fbs_encode_ns /
fbs_decode_ns / alloc_per_op / fbs_decode_allocs` 至 stdout 与 `bench/protocol.txt`。
指标实测值与口径见 `PERFORMANCE.md`。

## 手工复核清单

1. `git status` 不出现 `*.pb.h` / `*_generated.h`（生成代码不入库，§20.5）
2. `ctest -R Protocol` Debug / Release 双构建全绿
3. `grep -r "class.*Envelope" --include="*.h" | grep -v protocol/` 无第二套消息头（§21）
4. `schema/*.fbs` 与 init commit 逐字节一致（冻结件未被改动）
5. `flatbuf_codec.cpp` 的 Decode 路径无 `new` / `make_unique`（零拷贝红线）
