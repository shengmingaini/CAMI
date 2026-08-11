#pragma once
// ============================================================================
// data/data_service/data_client.h — [PRODUCTION] Data Service gRPC 客户端
// ----------------------------------------------------------------------------
// GameNode 侧使用: 经本客户端调用 Data Service, 绝不直连 MySQL/Redis (红线)。
// 仅在 CAMI_BUILD_MODULES=ON (vcpkg: grpc + protobuf) 下编译。
// ============================================================================
#ifdef CAMI_BUILD_MODULES

#include <memory>
#include <string>

#include "data_service.grpc.pb.h"   // protoc 代码生成

namespace cami {
namespace data {
namespace data_service {

// GameNode 经此读写玩家数据 (代理到 Data Service)
class DataClient {
public:
    // channel 例: "data-service:50051" (K8s 服务名或 IP:port)
    explicit DataClient(const std::string& target);

    // 返回 {found, payload, version}; found=false 表示无此玩家
    std::tuple<bool, std::string, uint64_t> Get(uint64_t player_id);

    // write_through=true 强一致直写; 默认写回 (异步落库)
    uint64_t Put(uint64_t player_id, const std::string& payload, bool write_through = false);

    // 乐观锁写: 返回 {ok, current_version}; ok=false 表示版本冲突
    std::pair<bool, uint64_t> Cas(uint64_t player_id, const std::string& payload,
                                  uint64_t expected_version);

    bool Delete(uint64_t player_id);

private:
    std::unique_ptr<CAMI::Data::DataService::StubInterface> stub_;
};

}  // namespace data_service
}  // namespace data
}  // namespace cami

#endif  // CAMI_BUILD_MODULES
