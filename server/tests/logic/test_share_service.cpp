#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "cache/cache_store.h"
#include "cache/redis_pool.h"
#include "dao_fixture.h"
#include "db/file_dao.h"
#include "db/share_dao.h"
#include "logic/share_service.h"

namespace {

const std::string kClientKey = "127.0.0.1";

bool redisEnabled() {
    const char* value = std::getenv("CM_TEST_REDIS");
    return value != nullptr && std::string(value) == "1";
}

std::string envOr(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string(fallback);
}

cm::cache::RedisOptions redisOptions() {
    cm::cache::RedisOptions options;
    options.host = envOr("CM_TEST_REDIS_HOST", "127.0.0.1");
    options.port = std::atoi(envOr("CM_TEST_REDIS_PORT", "6379").c_str());
    options.password = envOr("CM_TEST_REDIS_PASSWORD", "");
    options.db = std::atoi(envOr("CM_TEST_REDIS_DB", "15").c_str());
    options.connectTimeout = std::chrono::milliseconds(200);
    options.commandTimeout = std::chrono::milliseconds(300);
    options.minSize = 1;
    options.maxSize = 2;
    options.idleTimeout = std::chrono::milliseconds(60000);
    options.acquireTimeout = std::chrono::milliseconds(200);
    options.maxLifetime = std::chrono::milliseconds(1800000);
    return options;
}

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

class ShareServiceTest : public cm::test::DaoTestBase {
protected:
    void SetUp() override {
        cm::test::DaoTestBase::SetUp();
        if (m_pool == nullptr) {
            return;  // 基类已 GTEST_SKIP（缺 MySQL）
        }
        if (!redisEnabled()) {
            GTEST_SKIP() << "set CM_TEST_REDIS=1 (db 15) to run ShareService tests";
            return;
        }

        std::string error;
        m_redis = cm::cache::RedisPool::create(redisOptions(), &error);
        ASSERT_TRUE(m_redis) << error;
        m_cache.reset(new cm::cache::CacheStore(*m_redis, cm::cache::CacheStoreOptions()));
        m_service.reset(new cm::logic::ShareService(*m_pool, *m_cache, *m_redis));
        createUser();
    }

    void TearDown() override {
        // 先清 Redis 键，再释放缓存/连接，最后 shutdown（避免 wait_for 5s 兜底）。
        if (m_cache) {
            for (std::size_t i = 0; i < m_codes.size(); ++i) {
                m_cache->invalidate(cm::cache::CacheKey::share(m_codes[i]));
                m_cache->invalidate(cm::cache::CacheKey::shareMiss(m_codes[i]));
                m_cache->invalidate(cm::cache::CacheKey::shareFail(m_codes[i], kClientKey));
            }
            for (std::size_t i = 0; i < m_tickets.size(); ++i) {
                m_cache->invalidate(cm::cache::CacheKey::shareTicket(m_tickets[i]));
            }
        }
        m_service.reset();
        m_cache.reset();
        if (m_redis && !m_redisDown) {
            m_redis->shutdown();
        }
        m_redis.reset();
        cm::test::DaoTestBase::TearDown();
    }

    cm::protocol::ShareCreateRq makeCreateRq(std::uint64_t ufId,
                                             std::uint8_t expirePolicy,
                                             std::uint8_t accessMode,
                                             std::uint32_t reqId) {
        cm::protocol::ShareCreateRq rq;
        rq.ufId = ufId;
        rq.expirePolicy = expirePolicy;
        rq.accessMode = accessMode;
        rq.reqId = reqId;
        return rq;
    }

    cm::protocol::ShareOpenRq makeOpenRq(const std::string& code,
                                         const std::string& accessCode) {
        cm::protocol::ShareOpenRq rq;
        rq.shareCode = code;
        rq.accessCode = accessCode;
        return rq;
    }

    cm::protocol::ShareCancelRq makeCancelRq(const std::string& code) {
        cm::protocol::ShareCancelRq rq;
        rq.shareCode = code;
        return rq;
    }

    cm::db::BlobRow loadBlob(std::uint64_t fid) {
        cm::db::BlobRow blob;
        cm::db::FileDao dao(*m_conn);
        EXPECT_EQ(cm::ErrorCode::Ok, dao.getBlobById(fid, &blob));
        return blob;
    }

    void trackCode(const std::string& code) {
        if (!code.empty()) {
            m_codes.push_back(code);
        }
    }

    void trackTicket(const std::string& ticket) {
        if (!ticket.empty()) {
            m_tickets.push_back(ticket);
        }
    }

    std::shared_ptr<cm::cache::RedisPool> m_redis;
    std::unique_ptr<cm::cache::CacheStore> m_cache;
    std::unique_ptr<cm::logic::ShareService> m_service;
    std::vector<std::string> m_codes;
    std::vector<std::string> m_tickets;
    bool m_redisDown = false;
};

TEST_F(ShareServiceTest, ShareCodeAlphabetOnly) {
    const std::string alphabet = cm::db::ShareDao::codeAlphabet();
    for (int i = 0; i < 1000; ++i) {
        const std::string code = cm::logic::ShareService::generateCode();
        ASSERT_EQ(std::size_t(6), code.size()) << "code: " << code;
        for (std::size_t j = 0; j < code.size(); ++j) {
            EXPECT_NE(std::string::npos, alphabet.find(code[j]))
                << "confusable/illegal char in: " << code;
        }
    }
}

TEST_F(ShareServiceTest, ShareCodeCollisionRetry) {
    const std::uint64_t fid = createBlob(1024);
    ASSERT_GT(fid, std::uint64_t(0));
    const std::uint64_t ufId = createNode(m_uid, "collide.bin", fid);
    ASSERT_GT(ufId, std::uint64_t(0));

    // 预置 code=222222 制造冲突。
    {
        cm::db::ShareDao dao(*m_conn);
        cm::db::NewShare ns;
        ns.code = "222222";
        ns.userId = m_uid;
        ns.ufId = ufId;
        ns.fileId = fid;
        ns.isDir = false;
        ns.expirePolicy = 2;
        std::uint64_t sid = 0;
        ASSERT_EQ(cm::ErrorCode::Ok, dao.insert(ns, &sid));
        trackShare("222222");
    }

    // 注入生成器：第一次撞 222222，重试给 333333。
    int calls = 0;
    cm::logic::ShareService service(*m_pool, *m_cache, *m_redis,
                                    [&calls]() -> std::string {
                                        return calls++ == 0 ? std::string("222222")
                                                            : std::string("333333");
                                    });

    cm::protocol::ShareCreateRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              service.create(m_uid, makeCreateRq(ufId, 2, 0, 9001), &rs));
    EXPECT_EQ(0u, rs.code);
    EXPECT_EQ(std::string("333333"), rs.shareCode);
    trackShare(rs.shareCode);
    trackCode(rs.shareCode);
}

TEST_F(ShareServiceTest, ShareCreateIdempotent) {
    const std::uint64_t fid = createBlob(2048);
    ASSERT_GT(fid, std::uint64_t(0));
    const std::uint64_t ufId = createNode(m_uid, "idem.bin", fid);
    ASSERT_GT(ufId, std::uint64_t(0));

    const cm::protocol::ShareCreateRq rq = makeCreateRq(ufId, 2, 1, 7001);

    cm::protocol::ShareCreateRs first;
    ASSERT_EQ(cm::ErrorCode::Ok, m_service->create(m_uid, rq, &first));
    EXPECT_EQ(0u, first.code);
    EXPECT_EQ(std::size_t(6), first.shareCode.size());
    EXPECT_EQ(1, static_cast<int>(first.accessCodeRequired));
    EXPECT_EQ(std::size_t(4), first.accessCode.size());
    trackShare(first.shareCode);
    trackCode(first.shareCode);

    // 同一 req_id 重放：返回同一 share_code，不产生第二条分享。
    cm::protocol::ShareCreateRs second;
    ASSERT_EQ(cm::ErrorCode::Ok, m_service->create(m_uid, rq, &second));
    EXPECT_EQ(0u, second.code);
    EXPECT_EQ(first.shareCode, second.shareCode);
    EXPECT_EQ(first.accessCode, second.accessCode);
}

TEST_F(ShareServiceTest, ShareOpenNoAccessCode) {
    const std::uint64_t fid = createBlob(4096);
    ASSERT_GT(fid, std::uint64_t(0));
    const cm::db::BlobRow blob = loadBlob(fid);
    const std::uint64_t ufId = createNode(m_uid, "video.mp4", fid);
    ASSERT_GT(ufId, std::uint64_t(0));

    cm::protocol::ShareCreateRs created;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->create(m_uid, makeCreateRq(ufId, 2, 0, 7101), &created));
    EXPECT_EQ(0, static_cast<int>(created.accessCodeRequired));
    trackShare(created.shareCode);
    trackCode(created.shareCode);

    cm::protocol::ShareOpenRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->open(makeOpenRq(created.shareCode, ""), kClientKey, &rs));
    EXPECT_EQ(0u, rs.code);
    EXPECT_EQ(fid, rs.fId);
    EXPECT_EQ(std::string("video.mp4"), rs.fileName);
    EXPECT_EQ(std::uint64_t(4096), rs.size);
    EXPECT_EQ(blob.md5, rs.fileMd5);
    EXPECT_EQ(0, static_cast<int>(rs.isDir));
    EXPECT_EQ(0, static_cast<int>(rs.accessCodeRequired));
    EXPECT_GT(rs.expireAtMs, nowMs());
    EXPECT_EQ(std::size_t(64), rs.shareTicket.size());
    trackTicket(rs.shareTicket);
}

TEST_F(ShareServiceTest, ShareOpenWrongCodeLocks) {
    const std::uint64_t fid = createBlob(1024);
    ASSERT_GT(fid, std::uint64_t(0));
    const std::uint64_t ufId = createNode(m_uid, "locked.bin", fid);
    ASSERT_GT(ufId, std::uint64_t(0));

    cm::protocol::ShareCreateRs created;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->create(m_uid, makeCreateRq(ufId, 2, 1, 7201), &created));
    ASSERT_EQ(1, static_cast<int>(created.accessCodeRequired));
    ASSERT_EQ(std::size_t(4), created.accessCode.size());
    trackShare(created.shareCode);
    trackCode(created.shareCode);

    // 5 次错误：每次回 2004，计数累加。
    for (int i = 0; i < 5; ++i) {
        cm::protocol::ShareOpenRs rs;
        EXPECT_EQ(cm::ErrorCode::AccessCodeWrong,
                  m_service->open(makeOpenRq(created.shareCode, "0000"),
                                  kClientKey, &rs));
        EXPECT_EQ(2004u, rs.code);
    }

    // 第 6 次即使给正确提取码也被锁定，锁定期间不比较、直接 2004。
    cm::protocol::ShareOpenRs locked;
    EXPECT_EQ(cm::ErrorCode::AccessCodeWrong,
              m_service->open(makeOpenRq(created.shareCode, created.accessCode),
                              kClientKey, &locked));
    EXPECT_EQ(2004u, locked.code);
}

TEST_F(ShareServiceTest, ShareExpired) {
    const std::uint64_t fid = createBlob(1024);
    ASSERT_GT(fid, std::uint64_t(0));
    const std::uint64_t ufId = createNode(m_uid, "exp.bin", fid);
    ASSERT_GT(ufId, std::uint64_t(0));

    cm::protocol::ShareCreateRs created;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->create(m_uid, makeCreateRq(ufId, 1, 0, 7301), &created));
    trackShare(created.shareCode);
    trackCode(created.shareCode);

    // 把过期时间改到过去；缓存里还是有效元信息，先失效再访问。
    ASSERT_TRUE(execPreparedString(
        "UPDATE t_share SET s_expire_at = DATE_SUB(NOW(6), INTERVAL 1 HOUR) "
        "WHERE s_code = ?",
        created.shareCode));
    m_cache->invalidate(cm::cache::CacheKey::share(created.shareCode));

    cm::protocol::ShareOpenRs rs;
    EXPECT_EQ(cm::ErrorCode::ShareExpired,
              m_service->open(makeOpenRq(created.shareCode, ""), kClientKey, &rs));
    EXPECT_EQ(2003u, rs.code);
}

TEST_F(ShareServiceTest, ShareCancelIdempotent) {
    const std::uint64_t fid = createBlob(1024);
    ASSERT_GT(fid, std::uint64_t(0));
    const std::uint64_t ufId = createNode(m_uid, "cancel.bin", fid);
    ASSERT_GT(ufId, std::uint64_t(0));

    cm::protocol::ShareCreateRs created;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->create(m_uid, makeCreateRq(ufId, 2, 0, 7401), &created));
    trackShare(created.shareCode);
    trackCode(created.shareCode);

    cm::protocol::ShareCancelRs first;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->cancel(m_uid, makeCancelRq(created.shareCode), &first));
    EXPECT_EQ(0u, first.code);
    EXPECT_EQ(1, static_cast<int>(first.cancelled));
    EXPECT_EQ(0, static_cast<int>(first.already));

    cm::protocol::ShareCancelRs second;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->cancel(m_uid, makeCancelRq(created.shareCode), &second));
    EXPECT_EQ(0u, second.code);
    EXPECT_EQ(0, static_cast<int>(second.cancelled));
    EXPECT_EQ(1, static_cast<int>(second.already));

    // 取消后访问返回 2003。
    cm::protocol::ShareOpenRs opened;
    EXPECT_EQ(cm::ErrorCode::ShareExpired,
              m_service->open(makeOpenRq(created.shareCode, ""), kClientKey, &opened));
    EXPECT_EQ(2003u, opened.code);
}

// 补充第 9 例：对应验收标准「Redis 挂返回 4008」，带提取码分享 fail-closed。
TEST_F(ShareServiceTest, ShareOpenRedisDownRejectsAccessCode) {
    const std::uint64_t fid = createBlob(1024);
    ASSERT_GT(fid, std::uint64_t(0));
    const std::uint64_t ufId = createNode(m_uid, "redisdown.bin", fid);
    ASSERT_GT(ufId, std::uint64_t(0));

    cm::protocol::ShareCreateRs created;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->create(m_uid, makeCreateRq(ufId, 2, 1, 7601), &created));
    ASSERT_EQ(1, static_cast<int>(created.accessCodeRequired));
    trackShare(created.shareCode);
    trackCode(created.shareCode);

    // Redis 挂：带提取码分享 fail-closed 4008，不 fail-open。
    m_redis->shutdown();
    m_redisDown = true;

    cm::protocol::ShareOpenRs rs;
    EXPECT_EQ(cm::ErrorCode::RateLimitUnavailable,
              m_service->open(makeOpenRq(created.shareCode, created.accessCode),
                              kClientKey, &rs));
    EXPECT_EQ(4008u, rs.code);
}

TEST_F(ShareServiceTest, ShareAccessDoesNotChangeRefCount) {
    const std::uint64_t fid = createBlob(4096);
    ASSERT_GT(fid, std::uint64_t(0));
    const cm::db::BlobRow before = loadBlob(fid);
    const std::uint64_t ufId = createNode(m_uid, "ref.bin", fid);
    ASSERT_GT(ufId, std::uint64_t(0));

    cm::protocol::ShareCreateRs created;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->create(m_uid, makeCreateRq(ufId, 2, 0, 7501), &created));
    trackShare(created.shareCode);
    trackCode(created.shareCode);

    cm::protocol::ShareOpenRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->open(makeOpenRq(created.shareCode, ""), kClientKey, &rs));
    trackTicket(rs.shareTicket);

    const cm::db::BlobRow after = loadBlob(fid);
    EXPECT_EQ(before.refCount, after.refCount);
}

}  // namespace
