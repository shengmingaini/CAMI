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
        if (cur) {
            resp->set_version(cur->version);
        } else {
            // 本地无版本记录 (首读/回源场景): 从 DB 补权威版本并写回本地 (D6 方案 B)
            auto db = proxy_.LoadWithVersion(key);
            if (db) {
                resp->set_version(db->version);
                version_.UpsertVersion(key, db->payload, db->version);
            } else {
                resp->set_version(0);
            }
        }
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
    // D6 方案 B: MySQL 为权威版本源, CasStore 直接 DB 版本条件写 (affected_rows 裁决),
    // 多 Data Service 副本并发时由 DB 层拦截, 不再依赖进程内 VersionedStore。
    bool ok = proxy_.CasStore(key, val, req->expected_version());
    auto db = proxy_.LoadWithVersion(key);   // 取当前 DB 版本 (成功=新版本; 失败=冲突时当前版本)
    if (ok) {
        // DB 已落库: 仅同步缓存 (不标记 dirty, 避免二次异步落库把 version 再 +1)
        proxy_.PutCachedOnly(key, val);
        if (db) version_.UpsertVersion(key, db->payload, db->version);
        resp->set_ok(true);
        resp->set_version(db ? db->version : req->expected_version() + 1);
    } else {
        resp->set_ok(false);
        resp->set_version(db ? db->version : req->expected_version());
    }
    return grpc::Status::OK;
}

grpc::Status DataServiceImpl::BatchPut(grpc::ServerContext*, const CAMI::Data::BatchPutReq* req,
                                       CAMI::Data::BatchPutResp* resp) {
    uint32_t ok = 0, fail = 0;
    for (const auto& row : req->rows()) {
        std::string key = "player:" + std::to_string(row.player_id());
        std::string val(row.payload());
        // Put 语义 = 无条件写 (last-write-wins): 缓存 + 本地版本推进 + dirty 异步落库
        proxy_.Put(key, val);
        version_.Set(key, val);
        ++ok;
    }
    resp->set_ok_count(ok);
    resp->set_fail_count(fail);
    return grpc::Status::OK;
}

grpc::Status DataServiceImpl::Delete(grpc::ServerContext*, const CAMI::Data::DeleteReq* req,
                                     CAMI::Data::DeleteResp* resp) {
    std::string key = "player:" + std::to_string(req->player_id());
    proxy_.Delete(key);            // 双删: 清缓存 + BackingStore::Delete 删行 (player_state)
    version_.Erase(key);           // 清理本地版本记录, 防已删角色被旧版本号复活
    resp->set_ok(true);
    return grpc::Status::OK;
}

}  // namespace data_service
}  // namespace data
}  // namespace cami

#endif  // CAMI_BUILD_MODULES
