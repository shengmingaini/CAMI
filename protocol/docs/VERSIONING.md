# protocol · 版本与演进规则（VERSIONING）

> 本文件是 TASK-005 的硬性交付物。协议是全项目唯一真相源，演进必须遵守本文规则。

## 1. 当前版本

| 项 | 值 |
|---|---|
| 协议版本 | **1.0.0**（与 `protocol/flatbuffers/VERSION` 同源） |
| Envelope.version 字段 | `uint32`，取主版本号（当前 1） |
| 校验策略 | `EnvelopeValidator::Validate(view, expected_version)`：`version != expected` 一律返回 `VERSION_CONFLICT`，**禁止静默降级、禁止跳过校验** |

## 2. 演进规则（红线）

1. **只增不改不删**：新增字段只允许追加到消息末尾（Protobuf 按字段号、FlatBuffers 按 vtable 天然向后兼容）。
2. **禁改类型**：已发布字段的类型与定宽（`fixed64` / `uint64` / `int64` …）不得变更。
3. **禁重排 FBS 字段**：已发布的 `.fbs` 字段顺序写入后不得调整（FlatBuffers 线格式与字段顺序耦合）。
4. **禁改字段号**：`.proto` 字段号一经发布即为永久占位，废弃字段标记 `reserved`。
5. **枚举只追加**：`CommandKind` / `QueryKind` / `EventKind` / 联合成员只能追加新值，禁止改值 / 删值。
6. **版本升级流程**：`schema 变更 → bump VERSION → bump Envelope.version → 更新本文 + INTERFACE.md → 全量回归 protocol_test`。

## 3. 双载体一致性契约

同一 Envelope 体系有两套序列化载体，字段一一对应，版本同源：

| 载体 | 文件 | 用途 | payload 形态 |
|---|---|---|---|
| Protobuf | `proto/envelope.proto` | 低频：管理面 / 数据面 / 跨进程 gRPC | 不透明 `bytes` |
| FlatBuffers | `flatbuffers/envelope_transport.fbs` | 高频：移动 / AOI / 战斗 | 不透明 `[ubyte]` |
| FlatBuffers（业务载荷层） | `flatbuffers/schema/envelope.fbs`（冻结） | 业务联合契约（`payload:MessageBody` union） | typed union |

- `schema/*.fbs` 五件套为任务包冻结产物，本任务未改动其中任何文件；
  `envelope_transport.fbs` 为 additive 新增（传输层信封），字段名与 proto 一一映射。
- 视图层统一使用 `EnvelopeMessageType`（`mmo/protocol/message_type.h`），
  禁止下游直接依赖任一生成器的同名枚举。

## 4. 兼容性保证

- 旧版本解码新版本消息：追加字段被忽略（Protobuf 未知字段保留、FlatBuffers vtable 缺省）。
- 新版本解码旧版本消息：追加字段取默认值。
- 跨版本 **不允许**：`EnvelopeValidator` 拒绝一切 `version` 不匹配的消息，
  上游收到 `VERSION_CONFLICT` 后应走版本协商或拒绝连接，不得猜测语义。
