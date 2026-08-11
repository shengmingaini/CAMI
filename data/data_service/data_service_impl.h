#pragma once
// ============================================================================
// data/data_service/data_service_impl.h — [PRODUCTION] Data Service gRPC 服务端
// ----------------------------------------------------------------------------
// 实现 proto/data_service.proto 的 DataService::Service:
//   - 组合 CacheProxy (读穿/写回) + VersionedStore (乐观锁) + BackingStore (回源/落库)
//   - GameNode 只调本服务, 绝不直连 MySQL/Redis (架构红线)
// 仅在 CAMI_BUILD_MODULES=ON (vcpkg: grpc + protobuf) 下编译。
// ============================================================================
#ifdef CAMI_BUILD_MODULES

#include <memory>
#include <string>

#include "data/redis_proxy/cache_proxy.h"
#include "data/version/version.h"
#include "data_service.grpc.pb.h"   // protoc 代码生成 (见 data/CMakeLists.txt)

namespace cami {
namespace data {
namespace data_service {

class DataServiceImpl final : public CAMI::Data::DataService::Service {
public:
    // proxy/version 由 Data Service 进程注入:
    //   - proxy:   CacheProxy (串联 CacheBackend + BackingStore), 负责读穿回源 / 写回 / 双删;
    //              Data Service 进程构造 (见 data_service_main.cpp)。
    //   - version: VersionedStore (永远需要, 防并发覆盖)。
    // 这样 GameNode 只调本服务, 绝不直连 MySQL/Redis (架构红线)。
    DataServiceImpl(redis_proxy::CacheProxy& proxy,
                    version::VersionedStore& version)
        : proxy_(proxy), version_(version) {}

    grpc::Status Get(grpc::ServerContext* ctx, const CAMI::Data::GetReq* req,
                     CAMI::Data::GetResp* resp) override;

    grpc::Status Put(grpc::ServerContext* ctx, const CAMI::Data::PutReq* req,
                     CAMI::Data::PutResp* resp) override;

    grpc::Status Cas(grpc::ServerContext* ctx, const CAMI::Data::CasReq* req,
                     CAMI::Data::CasResp* resp) override;

    grpc::Status BatchPut(grpc::ServerContext* ctx, const CAMI::Data::BatchPutReq* req,
                          CAMI::Data::BatchPutResp* resp) override;

    grpc::Status Delete(grpc::ServerContext* ctx, const CAMI::Data::DeleteReq* req,
                        CAMI::Data::DeleteResp* resp) override;

private:
    redis_proxy::CacheProxy& proxy_;   // 读穿/写回/双删统一入口
    version::VersionedStore& version_; // 乐观锁 CAS
};

}  // namespace data_service
}  // namespace data
}  // namespace cami

#endif  // CAMI_BUILD_MODULES
