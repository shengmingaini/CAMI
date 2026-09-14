# protocol_codec · GDExtension(C++) 协议绑定（TASK-034）

把 TASK-005 的 `mmo::protocol::FlatbufCodec` 暴露为 Godot singleton `ProtocolCodec`，
供 GDScript（`ClientWorld` / `NetClient`）消费。**GDScript 禁止重写协议编解码**。

## 暴露接口

| 方法 | 说明 |
|---|---|
| `decode_envelope(bytes: PackedByteArray) -> Dictionary` | 解码信封，返回 `message_id / message_type / message_type_name / version / source / timestamp_ms / trace_id / request_id / payload / transaction_id / idempotency_key` |
| `decode_snapshot(bytes: PackedByteArray) -> Dictionary` | 解码 AOI 快照 = 信封字段 + `entities`（每项 `{id,x,y,z}`） |
| `encode_envelope(message_type: int, payload: PackedByteArray) -> PackedByteArray` | 用信封包裹 payload（`message_type` 取 `EnvelopeMessageType`：Unknown0/Command1/Query2/Event3/Response4/Heartbeat5） |

GDScript 侧通过 `Engine.has_singleton("ProtocolCodec")` 判存后调用。

## 已核验的真实签名（非推测）

- `ICodec::Decode(std::string_view) -> Result<OwnedEnvelope>`
- `OwnedEnvelope::view() -> const EnvelopeView&`
- `EnvelopeView`：`message_id / message_type(EnvelopeMessageType) / version / source / timestamp_ms / trace_id / request_id / payload / transaction_id / idempotency_key`
- `mmo::core::Result<T>`：`HasValue() / Value() / Err()`（`engine/core/include/mmo/core/error/result.h`）

## 构建

```bash
# 1) 取 godot-cpp 4.7.2
git clone --branch 4.7 https://github.com/godotengine/godot-cpp.git godot-cpp

# 2) 构建（需 C++17 工具链 + protocol 库已编译）
cd client/extensions/protocol_codec
scons godot_cpp=./godot-cpp target=template_debug
scons godot_cpp=./godot-cpp target=template_release
```

产物落在 `bin/`，由 `client/extensions/protocol_codec.gdextension` 指向。

## ⚠ 当前两个真实前置缺口

1. **godot-cpp 未就位**：`client/extensions/protocol_codec/godot-cpp` 不存在，本沙箱亦无
   C++ 工具链（`cl` / `g++` / `gcc` / `scons` / `cmake` 均缺失），因此**本绑定在此环境无法编译**。
   需要在带 MinGW-MSYS2 / MSVC 的构建机上执行上述 `scons`。

2. **AOI 快照 FlatBuffers 头文件缺失**：`protocol/flatbuffers/generated/cpp/` 目前只有
   `command / common / envelope / event / query` 五个 `_generated.h`，**没有 `aoi_generated.h`**，
   而 `aoi.fbs` 存在。因此 `decode_snapshot()` 的实体抽取被
   `MMO_PROTOCOL_HAS_AOI_SNAPSHOT` 宏保护，默认返回空 `entities`。
   等 `aoi.fbs` 生成 `aoi_generated.h` 后，用 `scons aoi_snapshot=1` 打开实体抽取。

`extension_api.json` 已由本机 Godot 4.7.2 生成于此目录（`--dump-extension-api`，6.9MB），
供 godot-cpp 生成绑定使用。
