#pragma once
// ============================================================================
// data/sync/kafka_flush.h — [PRODUCTION] Kafka 异步落库 + 死信队列 (Week4 D6-T3)
// ----------------------------------------------------------------------------
// 架构 §5 同步/异步分离:
//   - 战斗/移动走主循环同步执行 (不落 DB);
//   - 邮件/日志/玩家持久化走 Kafka 异步队列。
// 本文件把 D4 SyncManager 的"同步 Store"升级为"异步发布到 Kafka",
// 由独立 consumer 进程 (KafkaSinkWorker) 消费后写 MySQL (经 ShardingSphere 代理)。
//   - 发布失败 / 消费持久化失败 -> 进入死信 topic (<topic>.DLQ), 待人工/重试。
//
// 仅在 CAMI_BUILD_MODULES=ON (vcpkg: cppkafka / librdkafka) 下编译。
// 同步 Store 路径保留为回退 (SyncManager 离线/降级时仍可用)。
// ============================================================================
#ifdef CAMI_BUILD_MODULES

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <cppkafka/cppkafka.h>

namespace cami {
namespace data {
namespace sync {

// 单条落库消息: key=player_id, value=序列化后的玩家行 (protobuf/json blob)
struct FlushMessage {
    std::string key;
    std::string value;
    uint64_t version = 0;  // 乐观锁版本, 供 consumer 做 CAS 落库
};

// ---------------------------------------------------------------------------
// KafkaFlush: 生产者, 把 dirty 行发布到落库 topic
// ---------------------------------------------------------------------------
class KafkaFlush {
public:
    // brokers: "broker1:9092,broker2:9092"
    // topic:   落库 topic (如 "cami.player.flush")
    KafkaFlush(const std::string& brokers, const std::string& topic);
    ~KafkaFlush() = default;

    // 发布单条; 返回 false = 入队失败 (应触发同步回退或告警)
    bool Publish(const FlushMessage& msg);

    // 批量发布; 返回成功条数
    std::size_t PublishBatch(const std::vector<FlushMessage>& batch);

    // 刷新底层生产者的投递缓冲
    void Flush();

private:
    cppkafka::Producer producer_;
    std::string topic_;
};

// ---------------------------------------------------------------------------
// KafkaSinkWorker: 消费者, 从落库 topic 读消息并写 BackingStore
// ---------------------------------------------------------------------------
// 为解耦, 本类只负责"消费 -> 回调", 实际写库由注入的 SinkFunc 完成,
// 这样 BackingStore / VersionedStore 的 CAS 逻辑留在调用方 (Data Service),
// KafkaSinkWorker 不依赖 cami::data 内部类型。
// 用 std::function 以支持捕获 lambda (Data Service 侧用 [&] 捕获 version/store)。
using SinkFunc = std::function<bool(const FlushMessage& msg)>;  // 返回 false = 持久化失败 -> DLQ

class KafkaSinkWorker {
public:
    KafkaSinkWorker(const std::string& brokers, const std::string& topic,
                    const std::string& group_id, SinkFunc sink);
    ~KafkaSinkWorker();

    // 启动消费循环 (阻塞); 收到 Stop() 后退出
    void Run();
    void Stop();

private:
    cppkafka::Consumer consumer_;
    std::string topic_;
    std::string dlq_topic_;
    SinkFunc sink_;
    std::atomic<bool> running_{false};
    cppkafka::Producer dlq_producer_;  // 死信发布者
};

}  // namespace sync
}  // namespace data
}  // namespace cami

#endif  // CAMI_BUILD_MODULES
