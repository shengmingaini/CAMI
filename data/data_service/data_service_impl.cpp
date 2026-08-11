// data/data_service/data_service_impl.cpp — [PRODUCTION] Data Service gRPC 服务端实现
// 仅在 CAMI_BUILD_MODULES=ON 编译 (vcpkg: grpc + protobuf)。
#include "data/data_service/data_service_impl.h"

#ifdef CAMI_BUILD_MODULES

#include <string>

namespace cami {
namespace data {
namespace data_service {

grpc::Status DataServiceImpl::Get(grpc::ServerContext*, const CAMI::Data::GetReq* req,
                                  CAMI::Data::GetResp* resp) {
    std::string key = "player:" + std::to_string(req->player_id());
    auto v = proxy_.Get(key);            // Read-Through: 命中返回, 未命中由 CacheProxy 回源 BackingStore 并回填
    if (v) {
        resp->set_found(true);
        resp->set_payload(*v);
        auto cur = version_.Load(key);
        if (cur) resp->set_version(cur->version);
        return grpc::Status::OK;
    }
    resp->set_found(false);
    return grpc::Status::OK;
}

grpc::Status DataServiceImpl::Put(grpc::ServerContext*, const CAMI::Data::PutReq* req,
                                  CAMI::Data::PutResp* resp) {
    std::string key = "player:" + std::to_string(req->player_id());
    std::string val(req->payload());
    // 写回 (默认) 或直写 (强一致)
    redis_proxy::WritePolicy policy = req->write_through()
                                          ? redis_proxy::WritePolicy::WriteThrough
                                          : redis_proxy::WritePolicy::WriteBack;
    // 版本: Put 语义 = 无条件覆盖, 版本无条件 +1。
    // 用 Set (而非 Cas) —— Cas 在并发冲突时静默失败, 会造成"缓存已写新值但版本是旧值"的不一致。
    proxy_.Put(key, val, policy);
    uint64_t new_ver = version_.Set(key, val);
    resp->set_ok(true);
    resp->set_version(new_ver);
    return grpc::Status::OK;
}

grpc::Status DataServiceImpl::Cas(grpc::ServerContext*, const CAMI::Data::CasReq* req,
                                  CAMI::Data::CasResp* resp) {
    std::string key = "player:" + std::to_string(req->player_id());
    std::string val(req->payload());
    bool ok = version_.Cas(key, val, req->expected_version());  // 仅版本匹配才写
    resp->set_ok(ok);
    auto cur = version_.Load(key);
    resp->set_version(cur ? cur->version : req->expected_version());
    if (ok) {
        // CAS 成功才落缓存 (Write-Back, 由同步模块异步落库)
        proxy_.Put(key, std::move(val));
    }
    return grpc::Status::OK;
}

grpc::Status DataServiceImpl::BatchPut(grpc::ServerContext*, const CAMI::Data::BatchPutReq* req,
                                       CAMI::Data::BatchPutResp* resp) {
    uint32_t ok = 0, fail = 0;
    for (const auto& row : req->rows()) {
        std::string key = "player:" + std::to_string(row.player_id());
        std::string val(row.payload());
        auto cur = version_.Load(key);
        bool written = cur ? version_.Cas(key, val, cur->version) : (version_.Init(key, val), true);
        if (written) {
            proxy_.Put(key, std::move(val));
            ++ok;
        } else {
            ++fail;
        }
    }
    resp->set_ok_count(ok);
    resp->set_fail_count(fail);
    return grpc::Status::OK;
}

grpc::Status DataServiceImpl::Delete(grpc::ServerContext*, const CAMI::Data::DeleteReq* req,
                                     CAMI::Data::DeleteResp* resp) {
    std::string key = "player:" + std::to_string(req->player_id());
    proxy_.Delete(key);   // CacheProxy::Delete 同时清缓存 + 回源删 DB (双删)
    resp->set_ok(true);
    return grpc::Status::OK;
}

}  // namespace data_service
}  // namespace data
}  // namespace cami

#endif  // CAMI_BUILD_MODULES
