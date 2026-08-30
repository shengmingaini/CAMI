// engine/core/tests/error_test.cpp — TASK-001 单元测试
//
// 自包含测试 harness（不依赖 gtest）：纯 assert + 计数器，vcpkg 离线不可用时仍可验证。
// 覆盖：错误码往返、Result 语义、AndThen/Map 短路、Result<void>、MMO_TRY 三层透传、
// 失败路径零堆分配、4KB 超长 message 不溢出。

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/error/result.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>

// 红线合规输出通道：禁止 std::cout / printf / std::cerr，统一走 fwrite。
#include "test_print.h"

// ---- 全局分配计数器：度量失败路径堆分配 ----
namespace {
std::size_t g_alloc_count = 0;
}  // namespace

void* operator new(std::size_t n) {
    g_alloc_count++;
    void* p = std::malloc(n);
    if (!p) std::abort();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                    \
            ::mmo::core::test::ErrorFmt("FAIL @ %s:%d : %s\n", __FILE__,    \
                                        __LINE__, #cond);                  \
            failures++;                                                    \
        }                                                                  \
    } while (0)

int main() {
    int failures = 0;

    // ---- 1. ErrorCode 往返映射（全覆盖） ----
    for (int i = 0; i <= 8; ++i) {
        auto c = static_cast<mmo::core::ErrorCode>(i);
        const char* name = mmo::core::ToString(c);
        auto back = mmo::core::FromString(name);
        CHECK(back.has_value());
        CHECK(*back == c);
    }
    CHECK(!mmo::core::FromString("NOPE").has_value());
    CHECK(!mmo::core::FromString("999").has_value());
    CHECK(mmo::core::FromString("0").has_value());
    CHECK(*mmo::core::FromString("0") == mmo::core::ErrorCode::OK);

    // ---- 2. IsRetryable ----
    CHECK(mmo::core::IsRetryable(mmo::core::ErrorCode::TIMEOUT));
    CHECK(mmo::core::IsRetryable(mmo::core::ErrorCode::BUSY));
    CHECK(mmo::core::IsRetryable(mmo::core::ErrorCode::RATE_LIMITED));
    CHECK(!mmo::core::IsRetryable(mmo::core::ErrorCode::OK));
    CHECK(!mmo::core::IsRetryable(mmo::core::ErrorCode::NOT_FOUND));
    CHECK(!mmo::core::IsRetryable(mmo::core::ErrorCode::INTERNAL_ERROR));

    // ---- 3. Error 值类型 ----
    {
        mmo::core::Error e(mmo::core::ErrorCode::NOT_FOUND, "player 42 not exist",
                           mmo::core::domain::kData);
        CHECK(e.Code() == mmo::core::ErrorCode::NOT_FOUND);
        CHECK(e.Message() == "player 42 not exist");
        CHECK(e.Domain() == "data");
        CHECK(e.IsRetryable() == false);
        CHECK(e.ToString() == "data/NOT_FOUND: player 42 not exist");
    }
    {
        mmo::core::Error e(mmo::core::ErrorCode::TIMEOUT, "slow");
        CHECK(e.IsRetryable());
    }
    // 超长 message（4KB）不截断溢出
    {
        std::string big(4096, 'x');
        mmo::core::Error e(mmo::core::ErrorCode::INTERNAL_ERROR, big);
        CHECK(e.Message().size() == 4096);
        CHECK(e.Message() == big);
    }

    // ---- 4. Result<T> 成功/失败/移动/拷贝 ----
    {
        auto ok = mmo::core::Result<int>::Ok(7);
        CHECK(ok.HasValue());
        CHECK(static_cast<bool>(ok));
        CHECK(ok.Value() == 7);
        auto fail = mmo::core::Result<int>::Fail(
            mmo::core::Error(mmo::core::ErrorCode::BUSY, "x"));
        CHECK(!fail.HasValue());
        CHECK(!static_cast<bool>(fail));
        CHECK(fail.Err().Code() == mmo::core::ErrorCode::BUSY);
        CHECK(fail.ValueOr(99) == 99);
    }
    {
        auto a = mmo::core::Result<int>::Ok(3);
        auto b = std::move(a);
        CHECK(b.HasValue());
        CHECK(b.Value() == 3);
        // 移动后原对象处于有效但未定义状态（variant 仍持有某替代），仅验证不崩溃
        (void)a;
    }

    // ---- 5. AndThen 短路 ----
    {
        auto good = []() -> mmo::core::Result<int> {
            return mmo::core::Result<int>::Ok(1);
        };
        auto bad = []() -> mmo::core::Result<int> {
            return mmo::core::Result<int>::Fail(
                mmo::core::Error(mmo::core::ErrorCode::NOT_FOUND, "nf"));
        };
        auto chained = good().AndThen([](int v) {
            return mmo::core::Result<int>::Ok(v + 1);
        });
        CHECK(chained.HasValue());
        CHECK(chained.Value() == 2);
        auto chained2 = bad().AndThen([](int) {
            return mmo::core::Result<int>::Ok(5);
        });
        CHECK(!chained2.HasValue());
        CHECK(chained2.Err().Code() == mmo::core::ErrorCode::NOT_FOUND);
    }

    // ---- 6. Map ----
    {
        auto r = mmo::core::Result<int>::Ok(2).Map([](int v) { return v * 10; });
        CHECK(r.HasValue());
        CHECK(r.Value() == 20);
        auto rf = mmo::core::Result<int>::Fail(
                      mmo::core::Error(mmo::core::ErrorCode::BUSY, "x"))
                      .Map([](int v) { return v * 10; });
        CHECK(!rf.HasValue());
    }

    // ---- 7. Result<void> ----
    {
        auto okv = mmo::core::Result<void>::Ok();
        CHECK(okv.HasValue());
        auto failv = mmo::core::Result<void>::Fail(
            mmo::core::Error(mmo::core::ErrorCode::UNAUTHORIZED, "no"));
        CHECK(!failv.HasValue());
        CHECK(failv.Err().Code() == mmo::core::ErrorCode::UNAUTHORIZED);
    }

    // ---- 8. MMO_TRY 三层透传（repo -> service -> handler），错误码与 domain 原样透传 ----
    {
        auto repo = [](int id) -> mmo::core::Result<int> {
            if (id < 0)
                return mmo::core::Result<int>::Fail(mmo::core::Error(
                    mmo::core::ErrorCode::NOT_FOUND, "id", mmo::core::domain::kData));
            return mmo::core::Result<int>::Ok(id);
        };
        auto service = [&](int id) -> mmo::core::Result<int> {
            int v = MMO_TRY(repo(id));
            return mmo::core::Result<int>::Ok(v * 2);
        };
        auto handler = [&](int id) -> mmo::core::Result<int> {
            int v = MMO_TRY(service(id));
            return mmo::core::Result<int>::Ok(v + 1);
        };
        auto good = handler(5);
        CHECK(good.HasValue());
        CHECK(good.Value() == 11);  // (5*2)+1
        auto bad = handler(-1);
        CHECK(!bad.HasValue());
        CHECK(bad.Err().Code() == mmo::core::ErrorCode::NOT_FOUND);
        CHECK(bad.Err().Domain() == "data");
    }

    // ---- 9. 失败路径零堆分配（1e6 次，operator new 计数必须为 0） ----
    {
        g_alloc_count = 0;
        for (int i = 0; i < 1000000; ++i) {
            auto r = mmo::core::Result<int>::Fail(
                mmo::core::Error(mmo::core::ErrorCode::BUSY, "fail"));
            auto r2 = std::move(r).AndThen(
                [](int) { return mmo::core::Result<int>::Ok(1); });
            CHECK(!r2.HasValue());  // 防止被优化掉
        }
        CHECK(g_alloc_count == 0u);
    }

    if (failures == 0) {
        ::mmo::core::test::Error("ALL CORE_ERROR TESTS PASSED\n");
        return 0;
    }
    ::mmo::core::test::ErrorFmt("%d TEST(S) FAILED\n", failures);
    return 1;
}
