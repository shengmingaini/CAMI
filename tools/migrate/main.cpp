// tools/migrate/main.cpp
//
// TASK-028 §15.6 / §20.6 · Schema 迁移工具（up / status / dry-run）。
//
// 约束（§4）：**禁止手工改表**，所有 schema 变更只能经本工具执行。
// 约束（§15.6）：dry-run 必须**只读**，不得写库（不建 schema_migrations）。
//
// 用法：
//   mysql_migrate up        [options]   执行未应用的迁移
//   mysql_migrate status    [options]   列出各分片的迁移状态
//   mysql_migrate dry-run   [options]   打印将执行的迁移清单，不写库
//
// options:
//   --dir=PATH        迁移目录（默认 <cwd>/database/migrations）
//   --host=HOST       实例地址（默认 127.0.0.1）
//   --port=PORT       实例端口（默认 3306）
//   --user=USER       账号（默认 root）
//   --shards=N        分片数（默认 8，与 ShardConfig 初始容量一致）
//   --db-prefix=PFX   逻辑库名前缀（默认 mmo_shard → mmo_shard0..N-1）
//   --password-env=N  密码来源环境变量名；缺省为空 = 本地实例无密码。
//                     生产用 --password-env=MMORPG_MYSQL_PASSWORD（严格语义：名字给了但
//                     变量未设置即明确失败，禁止空密码兜底）。
//
// 退出码：0 成功；1 任一分片失败（错误信息打印到 stderr，禁止伪成功）。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "mmo/data/mysql/migration.h"
#include "mmo/data/mysql/mysql_client.h"
#include "mmo/data/mysql/mysql_config.h"

namespace {

namespace core = mmo::core;

using mmo::data::mysql::MigrationRunner;
using mmo::data::mysql::MySqlConfig;
using mmo::data::mysql::ShardEndpoint;

struct Options {
    std::string action;
    std::string dir{"database/migrations"};
    std::string host{"127.0.0.1"};
    std::uint16_t port{3306};
    std::string user{"root"};
    std::uint32_t shards{8};
    std::string db_prefix{"mmo_shard"};
    std::string password_env;  // 空 = 无密码
};

void Usage() {
    std::fprintf(stderr,
                 "usage: mysql_migrate <up|status|dry-run> [--dir=PATH] [--host=HOST]"
                 " [--port=PORT] [--user=USER] [--shards=N] [--db-prefix=PFX]"
                 " [--password-env=ENV_NAME]\n");
}

bool ParseU32(std::string_view s, std::uint32_t& out) {
    if (s.empty()) return false;
    std::uint32_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10u + static_cast<std::uint32_t>(c - '0');
    }
    out = v;
    return true;
}

bool ParseArgs(int argc, char** argv, Options& opt) {
    if (argc < 2) return false;
    opt.action = argv[1];
    if (opt.action != "up" && opt.action != "status" && opt.action != "dry-run") return false;

    for (int i = 2; i < argc; ++i) {
        const std::string_view a{argv[i]};
        const auto eq = a.find('=');
        if (a.rfind("--", 0) != 0 || eq == std::string_view::npos) {
            std::fprintf(stderr, "unknown argument: %.*s\n", static_cast<int>(a.size()), a.data());
            return false;
        }
        const std::string_view k = a.substr(2, eq - 2);
        const std::string_view v = a.substr(eq + 1);
        const std::string val{v};
        if (k == "dir") {
            opt.dir = val;
        } else if (k == "host") {
            opt.host = val;
        } else if (k == "user") {
            opt.user = val;
        } else if (k == "db-prefix") {
            opt.db_prefix = val;
        } else if (k == "password-env") {
            opt.password_env = val;
        } else if (k == "port") {
            std::uint32_t p = 0;
            if (!ParseU32(v, p) || p == 0 || p > 65535) return false;
            opt.port = static_cast<std::uint16_t>(p);
        } else if (k == "shards") {
            if (!ParseU32(v, opt.shards) || opt.shards == 0 || opt.shards > 1024) return false;
        } else {
            std::fprintf(stderr, "unknown option: --%.*s\n", static_cast<int>(k.size()), k.data());
            return false;
        }
    }
    return true;
}

ShardEndpoint EndpointFor(const Options& opt, std::uint32_t shard) {
    ShardEndpoint ep;
    ep.host = opt.host;
    ep.port = opt.port;
    ep.user = opt.user;
    ep.database = opt.db_prefix + std::to_string(shard);
    return ep;
}

int Fail(const char* what, const core::Error& e) {
    const std::string_view msg = e.Message();
    std::fprintf(stderr, "[migrate] %s: %.*s (code=%d)\n", what, static_cast<int>(msg.size()),
                 msg.data(), static_cast<int>(e.Code()));
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!ParseArgs(argc, argv, opt)) {
        Usage();
        return 1;
    }

    MySqlConfig cfg;
    cfg.password_env = opt.password_env;
    auto password = mmo::data::mysql::ResolveMySqlPassword(cfg);
    if (!password.HasValue()) return Fail("resolve password", password.Err());

    std::printf("[migrate] action=%s dir=%s shards=%u host=%s:%u prefix=%s user=%s\n",
                opt.action.c_str(), opt.dir.c_str(), opt.shards, opt.host.c_str(),
                static_cast<unsigned>(opt.port), opt.db_prefix.c_str(), opt.user.c_str());

    int rc = 0;
    for (std::uint32_t s = 0; s < opt.shards; ++s) {
        const ShardEndpoint ep = EndpointFor(opt, s);

        // up 需要目标库存在（迁移建表在库内）；status/dry-run 保持只读语义，
        // 库不存在时按「全部 pending」如实报告，不隐式建库。
        if (opt.action == "up") {
            auto created = mmo::data::mysql::EnsureDatabaseExists(ep, cfg, password.Value());
            if (!created.HasValue()) return Fail("ensure database", created.Err());
        }

        auto runner = MigrationRunner::Create(ep, cfg, password.Value());

        // 库不存在：status/dry-run 保持只读语义，不隐式建库，如实报告「全部 pending」
        // （Discover 只读文件系统，不触库，故用引导连接即可枚举迁移文件）。
        if (!runner.HasValue() && opt.action != "up") {
            ShardEndpoint boot = ep;
            boot.database.clear();
            auto boot_runner = MigrationRunner::Create(boot, cfg, password.Value());
            if (!boot_runner.HasValue()) {
                rc = Fail(("connect " + ep.database).c_str(), boot_runner.Err());
                continue;
            }
            auto files = boot_runner.Value().Discover(opt.dir);
            if (!files.HasValue()) {
                rc = Fail(("discover " + opt.dir).c_str(), files.Err());
                continue;
            }
            std::printf("--- shard %u (%s) --- [database not created yet; status=dry]\n", s,
                        ep.database.c_str());
            for (const auto& f : files.Value()) {
                std::printf("  [%-7s] %03u  %s\n", "pending", f.version, f.name.c_str());
            }
            if (opt.action == "dry-run") {
                for (const auto& f : files.Value()) {
                    std::printf("  would run %zu statement(s) from %s\n",
                                MigrationRunner::SplitStatements(f.sql).size(), f.name.c_str());
                }
                std::printf("pending_versions=%zu\n", files.Value().size());
            }
            continue;
        }

        if (!runner.HasValue()) {
            rc = Fail(("connect " + ep.database).c_str(), runner.Err());
            continue;
        }
        auto& run = runner.Value();

        if (opt.action == "dry-run") {
            auto text = run.DryRun(opt.dir);
            if (!text.HasValue()) {
                rc = Fail(("dry-run " + ep.database).c_str(), text.Err());
                continue;
            }
            std::printf("--- shard %u (%s) ---\n%s", s, ep.database.c_str(), text.Value().c_str());
            continue;
        }

        auto st = run.Status(opt.dir);
        if (!st.HasValue()) {
            rc = Fail(("status " + ep.database).c_str(), st.Err());
            continue;
        }

        if (opt.action == "status") {
            std::printf("--- shard %u (%s) ---\n", s, ep.database.c_str());
            for (const auto& m : st.Value()) {
                const char* state = m.applied ? "applied" : (m.failed ? "FAILED" : "pending");
                std::printf("  [%-7s] %03u  %s%s%s\n", state, m.version, m.name.c_str(),
                            m.failed ? "  error=" : "", m.failed ? m.error.c_str() : "");
            }
            continue;
        }

        auto applied = run.Up(opt.dir);
        if (!applied.HasValue()) {
            rc = Fail(("up " + ep.database).c_str(), applied.Err());
            continue;
        }
        std::printf("--- shard %u (%s): applied %zu migration(s)\n", s, ep.database.c_str(),
                    applied.Value().size());
        for (const auto v : applied.Value()) std::printf("      applied %03u\n", v);
    }

    if (rc == 0) std::printf("[migrate] OK\n");
    return rc;
}
