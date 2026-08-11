// data/data_service/data_service_main.cpp — [PRODUCTION] Data Service 独立进程入口
// 仅在 CAMI_BUILD_MODULES=ON 编译。部署: K8s 单例/多副本无状态, GameNode 经 gRPC 调用。
//
// 本进程组合:
//   - RedisBackend (缓存, 连 Redis Cluster)
//   - MySQLBackingStore (落库, 经 ShardingSphere 代理)
//   - VersionedStore (乐观锁, 防并发覆盖)
//   - CacheProxy (读穿/写回) 串联三者
//   - KafkaSinkWorker 消费 player-flush topic -> 写 MySQL (异步落库闭环)
#include "data/data_service/data_service_impl.h"
#include "data/data_service/data_client.h"
#include "data/mysql_proxy/mysql_backing_store.h"
#include "data/redis_proxy/redis_backend.h"
#include "data/sync/kafka_flush.h"
#include "data/version/version.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef CAMI_BUILD_MODULES

namespace {
// 全局关机标志。信号处理器只置标志 (POSIX async-signal-safe),
// 绝不在此调 gRPC Shutdown (非信号安全, 可能死锁)。主线程轮询后优雅关闭。
std::atomic<bool> g_stop_flag{false};
void CamiSignalHandler(int) {
    g_stop_flag.store(true);
}
}  // namespace

using namespace cami::data;

int main(int argc, char** argv) {
    const std::string redis_uri    = std::getenv("CAMI_REDIS_URI")    ? std::getenv("CAMI_REDIS_URI")    : "redis://127.0.0.1:6379";
    const std::string mysql_host   = std::getenv("CAMI_MYSQL_HOST")  ? std::getenv("CAMI_MYSQL_HOST")  : "127.0.0.1";
    const int         mysql_port   = std::getenv("CAMI_MYSQL_PORT")  ? std::stoi(std::getenv("CAMI_MYSQL_PORT")) : 3309;
    const std::string mysql_user   = std::getenv("CAMI_MYSQL_USER")  ? std::getenv("CAMI_MYSQL_USER")  : "root";
    const std::string mysql_pass   = std::getenv("CAMI_MYSQL_PASS")  ? std::getenv("CAMI_MYSQL_PASS")  : "root";
    const std::string mysql_db     = std::getenv("CAMI_MYSQL_DB")    ? std::getenv("CAMI_MYSQL_DB")    : "cami_db";
    const std::string kafka_brokers= std::getenv("CAMI_KAFKA_BROKERS")? std::getenv("CAMI_KAFKA_BROKERS"): "127.0.0.1:9092";
    const std::string listen_addr  = std::getenv("CAMI_DATA_SVC_ADDR")? std::getenv("CAMI_DATA_SVC_ADDR"): "0.0.0.0:50051";

    // 后端
    redis_proxy::RedisBackend cache(redis_uri);
    mysql_proxy::MySQLBackingStore store(mysql_host, static_cast<uint16_t>(mysql_port),
                                         mysql_user, mysql_pass, mysql_db);
    version::VersionedStore version;

    // 缓存代理 (默认写回)
    redis_proxy::CacheProxy proxy(cache, store, redis_proxy::WritePolicy::WriteBack);

    // D6-T3: Kafka 生产者, 把 Write-Back dirty 集合发布到落库 topic。
    // 每次 FlushDirty 取出 dirty {key,value} -> 拼 FlushMessage(带当前版本) -> 批量发布。
    sync::KafkaFlush kafka_flush(kafka_brokers, "cami.player.flush");
    proxy.SetAsyncFlushSink([&](const std::vector<std::pair<std::string, std::string>>& pairs) -> bool {
        if (pairs.empty()) return true;
        std::vector<sync::FlushMessage> msgs;
        msgs.reserve(pairs.size());
        for (const auto& kv : pairs) {
            sync::FlushMessage fm;
            fm.key = kv.first;
            fm.value = kv.second;
            auto cur = version.Load(kv.first);  // 附当前版本供 consumer CAS / 观测
            fm.version = cur ? cur->version : 0;
            msgs.push_back(std::move(fm));
        }
        return kafka_flush.PublishBatch(msgs) == msgs.size();
    });

    // 异步落库消费者: 消费 -> 调 VersionedStore.Cas + BackingStore.Store
    sync::KafkaSinkWorker::SinkFunc sink = [&](const sync::FlushMessage& msg) -> bool {
        auto cur = version.Load(msg.key);
        bool ok = cur ? version.Cas(msg.key, msg.value, cur->version)
                      : (version.Init(msg.key, msg.value), true);
        if (ok) store.Store(msg.key, msg.value);
        return ok;
    };
    sync::KafkaSinkWorker sink_worker(kafka_brokers, "cami.player.flush", "data-service-sink", sink);
    std::thread sink_thread([&]() { sink_worker.Run(); });

    // 周期落库线程: 每 30s 把 dirty 集合排干到 Kafka (架构 §4: 背包/货币 30s 批量落库)。
    // 用 condition_variable 等待, 停机时 notify 即时唤醒, 不必等满一个 30s sleep。
    std::mutex cv_mu;
    std::condition_variable flush_cv;
    std::thread flush_thread([&]() {
        std::unique_lock<std::mutex> lk(cv_mu);
        while (!flush_cv.wait_for(lk, std::chrono::seconds(30),
                                  [&] { return g_stop_flag.load(); })) {
            proxy.FlushDirty();
        }
    });

    // 优雅停机: SIGINT/SIGTERM -> 信号处理器只置 g_stop_flag (见上)
    std::signal(SIGINT, CamiSignalHandler);
    std::signal(SIGTERM, CamiSignalHandler);

    // gRPC 服务 (注入 CacheProxy: 读穿/写回/双删统一由它做; version 独立管乐观锁)
    DataServiceImpl service(proxy, version);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    if (!server) { std::cerr << "[data_service] failed to start gRPC server\n"; return 1; }
    std::cout << "[data_service] listening on " << listen_addr << std::endl;

    // 主线程轮询停机标志 (200ms), 收到信号后安全地调用 Shutdown (主线程上下文, 非信号处理器)
    while (!g_stop_flag.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    server->Shutdown();   // 触发 RPC 排空, Wait 返回
    server->Wait();

    // 优雅停机: 唤醒周期落库线程 -> 最后排干一次 (避免丢最近 30s 的 dirty)
    flush_cv.notify_all();
    if (flush_thread.joinable()) flush_thread.join();
    proxy.FlushDirty();

    sink_worker.Stop();
    if (sink_thread.joinable()) sink_thread.join();
    return 0;
}

#endif  // CAMI_BUILD_MODULES
