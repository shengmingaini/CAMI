// data/sync/kafka_flush.cpp — [PRODUCTION] Kafka 异步落库 + 死信队列实现
// 仅在 CAMI_BUILD_MODULES=ON 编译 (vcpkg: cppkafka / librdkafka)。
#include "data/sync/kafka_flush.h"

#ifdef CAMI_BUILD_MODULES

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace cami {
namespace data {
namespace sync {

// ---------------- KafkaFlush (生产者) ----------------
KafkaFlush::KafkaFlush(const std::string& brokers, const std::string& topic)
    : producer_(cppkafka::Configuration{{"metadata.broker.list", brokers}}),
      topic_(topic) {
    producer_.set_payload_policy(cppkafka::Producer::PayloadPolicy::BLOCK_ON_FULL_QUEUE);
}

bool KafkaFlush::Publish(const FlushMessage& msg) {
    try {
        cppkafka::MessageBuilder builder(topic_);
        builder.key(msg.key);
        builder.payload(msg.value);
        // 版本号编码进消息头, 供 consumer CAS 落库
        builder.header("version", std::to_string(msg.version));
        producer_.produce(builder);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[KafkaFlush] publish failed: " << e.what() << std::endl;
        return false;
    }
}

std::size_t KafkaFlush::PublishBatch(const std::vector<FlushMessage>& batch) {
    std::size_t ok = 0;
    for (const auto& m : batch) if (Publish(m)) ++ok;
    Flush();
    return ok;
}

void KafkaFlush::Flush() {
    try { producer_.flush(); } catch (const std::exception& e) {
        std::cerr << "[KafkaFlush] flush failed: " << e.what() << std::endl;
    }
}

// ---------------- KafkaSinkWorker (消费者 + DLQ) ----------------
KafkaSinkWorker::KafkaSinkWorker(const std::string& brokers, const std::string& topic,
                                 const std::string& group_id, SinkFunc sink)
    : consumer_(cppkafka::Configuration{
          {"metadata.broker.list", brokers},
          {"group.id", group_id},
          {"enable.auto.commit", false}}),
      topic_(topic),
      dlq_topic_(topic + ".DLQ"),
      sink_(sink),
      dlq_producer_(cppkafka::Configuration{{"metadata.broker.list", brokers}}) {}

KafkaSinkWorker::~KafkaSinkWorker() { Stop(); }

void KafkaSinkWorker::Stop() {
    running_ = false;
    try { consumer_.unsubscribe(); } catch (...) {}
}

void KafkaSinkWorker::Run() {
    running_ = true;
    consumer_.subscribe({topic_});
    while (running_) {
        auto msg = consumer_.poll(std::chrono::milliseconds(100));
        if (!msg) continue;

        FlushMessage fm;
        fm.key = msg.get_key();
        fm.value = msg.get_payload();
        // 版本号从头中取回
        auto vh = msg.get_header("version");
        if (vh) fm.version = std::strtoull(vh->get_value().data(), nullptr, 10);

        bool persisted = sink_ ? sink_(fm) : false;
        if (!persisted) {
            // 持久化失败 -> 死信 topic, 保留原 key/value/version 供排查
            try {
                cppkafka::MessageBuilder b(dlq_topic_);
                b.key(fm.key);
                b.payload(fm.value);
                b.header("version", std::to_string(fm.version));
                b.header("reason", "sink_failed");
                dlq_producer_.produce(b);
                dlq_producer_.flush();
                persisted = true;  // 移交 DLQ 成功即视为已处理: 提交 offset, DLQ 独立重试, 主链路不卡
            } catch (const std::exception& e) {
                std::cerr << "[KafkaSinkWorker] DLQ publish failed: " << e.what()
                          << std::endl;
            }
        }
        // at-least-once: 成功落库 或 成功移交 DLQ 才提交 offset;
        // 两者都失败 -> 不提交, 重启从该 offset 重放 (数据仍在主 topic, 不丢)。
        if (persisted) consumer_.commit(msg);
    }
}

}  // namespace sync
}  // namespace data
}  // namespace cami

#endif  // CAMI_BUILD_MODULES
