# protocol — 协议模块（TASK-005）

全项目唯一的消息契约与编解码层：**MessageEnvelope / Command / Query / Event 的
Protobuf 定义 + 高频游戏数据的 FlatBuffers 定义**，C++ 侧提供
Encode / Decode / Validate / Version Check。

## 目录结构

```
protocol/
├── proto/                  # Protobuf 契约（package mmo.protocol）
│   ├── common.proto        #   Vec3 / Vec2 / EntityRef
│   ├── command.proto       #   CommandKind + oneof body（最小集合，后续增量扩展）
│   ├── query.proto         #   QueryKind + oneof body
│   ├── event.proto         #   EventKind + oneof body
│   └── envelope.proto      #   MessageEnvelope（10 字段，含经济扩展字段）
├── flatbuffers/            # FlatBuffers 契约
│   ├── schema/             #   冻结五件套（common/envelope/command/query/event，勿改）
│   ├── movement.fbs        #   热路径帧 MOV0
│   ├── aoi.fbs             #   热路径帧 AOI0
│   ├── combat.fbs          #   热路径帧 CMB0
│   └── envelope_transport.fbs  # 传输层信封（TransportEnvelope，MSG0）
├── include/mmo/protocol/   # 公开接口（唯一对外入口）
├── src/codec/              # 实现（下游禁止 include）
├── tests/                  # protocol_test / protocol_bench
└── docs/                   # 五文档契约
```

## 关键设计

- **一套消息头**：所有 C/Q/E 走统一 Envelope（规范 §5）；下游禁止另起消息头。
- **双载体**：Protobuf（低频管理面）+ FlatBuffers（高频移动/AOI/战斗），
  字段一一映射，版本同源（见 `docs/VERSIONING.md`）。
- **零拷贝解码**：`FlatbufCodec::Decode` 全程零堆分配（实测 `fbs_decode_allocs=0`）。
- **版本硬校验**：`version != expected` 一律 `VERSION_CONFLICT`，不静默降级。
- **payload 上限**：1 MiB（`kMaxPayloadBytes`）。

## 构建

由根 `CMakeLists.txt` 的 `MMORPG_SUBDIRS` 自动纳入；构建期调用 `protoc` / `flatc`
生成代码到 `build/generated/`（不入库）。

```bash
cmake --build build/Release --target protocol_test protocol_bench
ctest --test-dir build/Release -R Protocol
```

## 文档

| 文档 | 内容 |
|---|---|
| `docs/INTERFACE.md` | 公开接口与语义约定 |
| `docs/VERSIONING.md` | 版本与演进规则（硬性交付物） |
| `docs/DEPENDENCY.md` | 依赖方向与工具链 |
| `docs/PERFORMANCE.md` | 实测性能数据 |
| `docs/TEST.md` | 测试用例与手工复核清单 |
