# protocol · 公开接口（INTERFACE）

> 下游只能通过 `protocol/include/mmo/protocol/` 下的公开头调用；禁止 include 本模块 `src/`。

## 头文件索引

| 头文件 | 内容 |
|---|---|
| `mmo/protocol/message_type.h` | `EnvelopeMessageType`（视图层枚举）+ `ToString()` |
| `mmo/protocol/codec/envelope_view.h` | `EnvelopeView`（解码零拷贝视图，全 `string_view` 借用） |
| `mmo/protocol/codec/owned_envelope.h` | `OwnedEnvelope`（持有底层表示 + 视图） |
| `mmo/protocol/codec/icodec.h` | `ICodec` 抽象接口 |
| `mmo/protocol/codec/protobuf_codec.h` | `ProtobufCodec`（低频：管理面 / 数据面） |
| `mmo/protocol/codec/flatbuf_codec.h` | `FlatbufCodec`（高频：移动 / AOI / 战斗；解码零拷贝） |
| `mmo/protocol/codec/envelope_validator.h` | `EnvelopeValidator` + `kMaxPayloadBytes`（1 MiB） |

## 核心签名

```cpp
namespace mmo::protocol {

enum class EnvelopeMessageType : uint8_t { Unknown=0, Command=1, Query=2, Event=3, Response=4, Heartbeat=5 };

struct EnvelopeView {            // 解码后的零拷贝视图；底层缓冲须在视图存活期内有效
  uint64_t            message_id;
  EnvelopeMessageType message_type;
  uint32_t            version;
  std::string_view    source, trace_id, payload, transaction_id, idempotency_key;
  int64_t             timestamp_ms;
  uint64_t            request_id;
};

class ICodec {
public:
  virtual ~ICodec() = default;
  virtual Result<std::vector<uint8_t>> Encode(const EnvelopeView&) const = 0;
  virtual Result<OwnedEnvelope>        Decode(std::string_view bytes) const = 0;
};

class EnvelopeValidator {
public:
  static Result<void> Validate(const EnvelopeView&, uint32_t expected_version);
};

}  // namespace mmo::protocol
```

## 语义约定

- **Codec 无状态、线程安全**：实例可并发复用；`Encode`/`Decode` 无共享可变状态。
- **Envelope 构造后只读**：禁止在传输链路中途改写任一字段（规范 §4 State Owner）。
- **错误码**（来自 `mmo::core`，禁止自定义第二套）：
  - `INVALID_ARGUMENT`：未知 message_type / 空 source / C·Q·E 空 payload / payload 超限 / 畸形字节流
  - `VERSION_CONFLICT`：`version != expected_version`（不静默降级）
- **payload 上限**：`kMaxPayloadBytes = 1 MiB`（战斗帧另有专帧，不走 Envelope 大载荷）。
- **零拷贝口径**：`FlatbufCodec::Decode` 全程零堆分配（bench 实测 `fbs_decode_allocs=0`），
  `EnvelopeView` 各 `string_view` 直接指向输入缓冲。

## 使用示例

```cpp
mmo::protocol::FlatbufCodec codec;                // 高频路径
mmo::protocol::EnvelopeView in = ...;             // 填充信封
auto wire = codec.Encode(in);                     // Result<vector<uint8_t>>
if (wire.HasValue()) {
  auto back = codec.Decode({reinterpret_cast<const char*>(wire.Value().data()),
                            wire.Value().size()});
  if (back.HasValue()) {
    auto& v = back.Value().view();                // 零拷贝视图
    // v.message_type / v.payload ...
  }
}
```

## schema / proto 资产

| 资产 | 说明 |
|---|---|
| `proto/{common,command,query,event,envelope}.proto` | 跨进程 Protobuf 契约（`package mmo.protocol`） |
| `flatbuffers/schema/*.fbs`（冻结） | 业务载荷层契约（`Vec3` / `Command`+union / `Envelope` union 版） |
| `flatbuffers/{movement,aoi,combat}.fbs` | 热路径帧（MOV0 / AOI0 / CMB0） |
| `flatbuffers/envelope_transport.fbs` | 传输层信封（`TransportEnvelope`，字段与 proto 一一映射） |

生成代码（`*.pb.h` / `*_generated.h`）输出到 `build/generated/`，**禁止入库、禁止手改**。
