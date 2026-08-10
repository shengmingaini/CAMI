# 协议编解码模块（codec）详细设计

> **文档状态**: [PROTOTYPE]  
> **版本**: v1.0.0  
> **更新日期**: 2026-08-10（Week2 周二 — 网关·协议编解码）  
> **所属层**: 接入层（gateway/codec）  
> **上游规约**: `docs/architecture/architecture-spec.md` (v3.0.0, 16 ADR)  
> **协议契约**: `docs/protocols/protocol-manifest-v1.md` + `proto/flatbuffers/envelope.fbs`  
> **关联 ADR**: ADR-002（模块边界）、架构 §4.1（接入层职责）

---

## 1. 模块概述

- **定位**：网关接入层（可靠链路 轨道 B）的协议编解码模块，负责客户端 ↔ Gateway 之间**长度前缀帧**的封包/解包与 TCP 字节流的定界（防粘包/半包/超大包）。
- **核心职责**：
  1. `encode_frame`：将一段 payload（FlatBuffers 二进制）包成 `[uint32 LE 长度][payload]`。
  2. `FrameDecoder::consume`：增量消费 socket 原始字节，按长度前缀切出完整帧并回调交付；处理粘包（循环切帧）、半包（累积等待）、超大包（超上限直接拒绝并清空）。
  3. `looks_like_flatbuffer`：轻量 FlatBuffers 结构校验（根偏移范围），作为深度校验前的廉价防线。
- **不在职责内（边界）**：
  - **不解析 FlatBuffer 内部**（不依赖 flatbuffers 库）；真正的 (de)serialization 由协议层（`envelope.fbs` + flatc，在 `CAMI_BUILD_MODULES=ON` 时）负责。
  - **不拥有 socket / 不调用 Asio**：codec 是纯缓冲级组件，由 Connection 在 read 循环里喂字节。
  - **不做加密/压缩/路由**：分别归 security / （轨道 A 位压缩 v2.0）/ router 模块。

## 2. 架构约束与边界

- **是否拥有 `Player` 对象**：否（接入层红线，架构 §1.3）。
- **红线禁令**：禁止 JSON 高频（架构 §1.3，已全部二进制）；禁止在编解码热路径分配大块内存（超大包直接拒绝，零分配）；禁止模块间直接访问内部成员（ADR-002）。
- **零外部依赖**：纯 `std` 实现 → 满足"模块独立编译"验收（`CAMI_BUILD_MODULES=OFF` 即可进 CI）。

## 3. 对外接口（C++ 签名级）

### 3.1 封包（encode）
```cpp
namespace cami::gateway::codec;
std::vector<uint8_t> encode_frame(const uint8_t* payload, size_t len);   // 纯函数
std::vector<uint8_t> encode_frame(const std::vector<uint8_t>& payload); // 便捷重载
```

### 3.2 解包（decode，增量）
```cpp
class FrameDecoder {
public:
    explicit FrameDecoder(size_t max_frame_size = 65536);
    DecodeResult consume(const uint8_t* data, size_t len,
                         const std::function<void(std::vector<uint8_t>&&)>& on_frame);
    void reset();
    size_t buffered_bytes() const;
};
// DecodeResult: kOk（0+ 帧交付，残流保留） / kOversized（声明长度越界，缓冲清空，调用方应关连接）
```

### 3.3 结构校验
```cpp
bool looks_like_flatbuffer(const uint8_t* data, size_t len); // 仅查根偏移范围，不依赖库
```

## 4. 核心数据结构 / 线格式

线格式（小端）：
```
 0               4
 +---------------+----------------------------+
 | uint32 length | payload (FlatBuffer 二进制) |
 +---------------+----------------------------+
 length = payload 字节数（不含 4 字节头）。
 payload = 一条 FlatBuffers 二进制（MessageEnvelope，envelope.fbs）。
```
- `kFrameHeaderSize = 4`
- `kDefaultMaxFrameSize = 65536`（单帧 payload 上限，可配；生产按最大协议消息体调）
- `FrameDecoder` 内部 `buf_` 为累积缓冲，`consume` 用滑动偏移 `i` 切帧，已消费前缀在末尾 erase（帧 ≤64KiB，O(n) 擦除开销可忽略，符合 [PROTOTYPE] 性能预算）。

## 5. 并发模型

- **线程归属**：单连接单线程消费。每个 `Connection` 在同一个 io_context 线程里 `async_read` → 喂 `FrameDecoder` → `on_frame` 回调，全程同线程，无锁（热路径零开销，符合架构 §3.3 / 无全局锁红线）。
- **跨连接**：各连接独立 `FrameDecoder` 实例，彼此不共享状态。
- **不内置锁**：刻意不为 decoder 加锁（避免热路径开销）；若未来需要多线程喂同一 decoder，由调用方保证串行。

## 6. 性能预算

| 指标 | 红线/目标 | 本模块保障 |
|------|-----------|-----------|
| 单帧解码 | O(帧长)，payload 拷贝一次 | ✅ |
| 超大包防护 | 零分配、立即拒绝 | ✅（不 memcpy、不 reserve 巨块） |
| 内存 | 缓冲 ≤ 一个半包（≤ max） | ✅ |

## 7. 依赖方向

```
[Connection (day1)] ──feed bytes──▶ [codec::FrameDecoder] ──on_frame(payload)──▶ [router 模块 (待建)]
[codec::encode_frame] ◀── 协议层 (envelope.fbs/flatc, ON) 产出的 FlatBuffer 二进制
```
- **上游（调用本模块）**：Connection（喂原始字节）、协议层（产出 payload）。
- **下游（本模块调用）**：无（纯 std）。

## 8. 集成缝（与周一 Connection 的接合点，本周不实现）

周一 `Connection` 已有 `on_message` 钩子但未做字节读循环。预期集成（**后续任务，不在周二范围**）：
```cpp
// 在 Connection::do_read() 中（伪代码）：
void Connection::do_read() {
    socket_.async_read_some(buf, [this](error_code ec, size_t n){
        if (ec) return close();
        decoder_.consume(buf.data(), n, [this](std::vector<uint8_t>&& frame){
            // frame 为一条 FlatBuffer 二进制 → 交给 router 模块 → 事件总线分发
            router_.on_frame(connection_id_, std::move(frame));
        });
        if (last_result == DecodeResult::kOversized) return close();  // 失同步，踢线
        do_read();
    });
}
```
> 本周仅交付 codec 模块 + 单测，不改动 Connection，避免 scope creep 与破坏已绿构建。

## 9. 协议引用

| 方向 | 内容 | 来源 |
|------|------|------|
| 可靠链路帧 | `[uint32 长度][FlatBuffer]` | 本模块定义 |
| payload 结构 | `MessageEnvelope`（含 `MessageBody` union、`proto_version`） | `proto/flatbuffers/envelope.fbs` |

- 低频可靠消息统一封入 `MessageEnvelope`（envelope.fbs），类型由 `body_type` 判别。
- 高频逐帧移动/朝向走**轨道 A 位压缩**（v2.0，不在本模块），不携 FlatBuffers 表。

## 10. 关联 ADR / 架构章节

| 决策 | 编号 |
|------|------|
| 模块边界 WoW 模式（不拥有 Player） | ADR-002 |
| 接入层职责（连接维持/编解码/路由，零持久化） | 架构 §4.1 |

## 11. 开放问题 / 后续

- **v2.0 轨道 A 位压缩**：高频 UDP 消息的位压缩编解码（架构 §96/844），独立于本模块，后续任务。
- **魔数/版本头**：当前为纯长度前缀，未加 magic/version。生产建议加 2 字节 magic + 1 字节 version 提升失步自愈能力（需在协议层定版后统一）。
- **深度 FlatBuffers 校验**：`looks_like_flatbuffer` 仅查根偏移；vtable/字段级校验由协议层（flatc 生成，`CAMI_BUILD_MODULES=ON`）负责，本模块不重复。
- **消息聚合（5ms 窗口批量打包）**：架构 §96 v3.0，未来在编码侧叠加，不影响帧格式。
