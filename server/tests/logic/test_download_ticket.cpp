#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "cache/cache_store.h"
#include "cache/redis_pool.h"
#include "logic/download_ticket.h"

namespace {

bool redisEnabled() {
    const char* value = std::getenv("CM_TEST_REDIS");
    return value != NULL && std::string(value) == "1";
}

std::string envOr(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != NULL ? std::string(value) : std::string(fallback);
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

std::string ticketKey(const std::string& token) {
    return cm::cache::CacheKey::downloadTicket(token);
}

// PTTL（毫秒）；Redis 返回 -2 表示键不存在，-1 表示无过期时间。
long long pttlOf(cm::cache::RedisPool& pool, const std::string& token) {
    std::unique_ptr<cm::cache::RedisConn> conn = pool.acquire();
    if (!conn) {
        return -2;
    }
    cm::cache::RedisReply reply;
    if (!conn->eval("return redis.call('PTTL', KEYS[1])", ticketKey(token), "",
                   &reply)) {
        return -2;
    }
    if (!reply.valid() || reply.isError()) {
        return -2;
    }
    return reply.integer();
}

// 直接改写票据 value（模拟过期 / 缩短 hard_exp），TTL 保持 5min。
bool overwriteTicket(cm::cache::RedisPool& pool, const std::string& token,
                     std::uint64_t uid, std::uint64_t fid,
                     std::uint64_t hardExpMs) {
    std::unique_ptr<cm::cache::RedisConn> conn = pool.acquire();
    if (!conn) {
        return false;
    }
    const std::string value = std::to_string(uid) + "|" +
                              std::to_string(fid) + "|" +
                              std::to_string(hardExpMs) + "|0";
    return conn->setPx(ticketKey(token), value,
                       std::chrono::milliseconds(300000));
}

// 把 TTL 压到 seconds，模拟“5min 已流逝”。
bool shrinkTtl(cm::cache::RedisPool& pool, const std::string& token,
               int seconds) {
    std::unique_ptr<cm::cache::RedisConn> conn = pool.acquire();
    if (!conn) {
        return false;
    }
    return conn->expire(ticketKey(token), std::chrono::seconds(seconds));
}

class DownloadTicketTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!redisEnabled()) {
            GTEST_SKIP() << "set CM_TEST_REDIS=1 (db 15) to run DownloadTicket tests";
            return;
        }

        std::string error;
        m_redis = cm::cache::RedisPool::create(redisOptions(), &error);
        ASSERT_TRUE(m_redis) << error;
        m_ticket.reset(new cm::logic::DownloadTicket(*m_redis));
    }

    void TearDown() override {
        m_ticket.reset();
        if (m_redis && !m_down) {
            {
                std::unique_ptr<cm::cache::RedisConn> conn = m_redis->acquire();
                if (conn) {
                    for (std::size_t i = 0; i < m_tokens.size(); ++i) {
                        conn->del(ticketKey(m_tokens[i]));
                    }
                }
            }
            // 连接必须在上面的作用域结束时归还（inUse 归零），否则 shutdown
            // 会 wait_for(5s, inUse==0) 硬等满 5 秒。
            m_redis->shutdown();
        }
        m_redis.reset();
    }

    // 签发并登记，TearDown 统一清理。
    std::string issueOk(std::uint64_t uid, std::uint64_t fid) {
        std::string token;
        std::uint64_t expireAtMs = 0;
        EXPECT_EQ(cm::ErrorCode::Ok,
                  m_ticket->issue(uid, fid, &token, &expireAtMs));
        EXPECT_EQ(std::size_t(64), token.size());
        EXPECT_EQ(std::string::npos, token.find_first_not_of("0123456789abcdef"));
        EXPECT_GT(expireAtMs, nowMs());
        m_tokens.push_back(token);
        return token;
    }

    std::shared_ptr<cm::cache::RedisPool> m_redis;
    std::unique_ptr<cm::logic::DownloadTicket> m_ticket;
    std::vector<std::string> m_tokens;
    bool m_down = false;
};

TEST_F(DownloadTicketTest, TicketIssueAndVerify) {
    const std::string token = issueOk(1001, 2002);

    cm::logic::DownloadTicketInfo info;
    ASSERT_EQ(cm::ErrorCode::Ok, m_ticket->verify(token, 1001, 2002, &info));
    EXPECT_EQ(std::uint64_t(1001), info.uid);
    EXPECT_EQ(std::uint64_t(2002), info.fid);
    EXPECT_EQ(0u, info.flags);
    EXPECT_GT(info.hardExpMs, nowMs() + 7000000ULL);  // 硬上限约 2h
    EXPECT_LT(info.hardExpMs, nowMs() + 7300000ULL);
    EXPECT_GT(pttlOf(*m_redis, token), 299000);
    EXPECT_LE(pttlOf(*m_redis, token), 300000);

    ASSERT_TRUE(shrinkTtl(*m_redis, token, 5));
    ASSERT_EQ(cm::ErrorCode::Ok, m_ticket->verify(token, 1001, 2002, &info));
    EXPECT_LE(pttlOf(*m_redis, token), 5000);  // verify 不续期

    std::unique_ptr<cm::cache::RedisConn> conn = m_redis->acquire();
    ASSERT_TRUE(conn);
    const char* malformed[] = {
        "1001|2002|18446744073709551616|0",
        "1001|2002|-1|0",
        "1001|2002|+9999999999999|0",
        "1001|2002| 9999999999999|0",
        "1001|2002|9999999999999|0|extra"
    };
    for (const auto& value : malformed) {
        ASSERT_TRUE(conn->setPx(ticketKey(token), value,
                                std::chrono::milliseconds(300000)));
        EXPECT_EQ(cm::ErrorCode::PermissionDenied,
                  m_ticket->verify(token, 1001, 2002, &info)) << value;
        EXPECT_EQ(cm::ErrorCode::PermissionDenied,
                  m_ticket->touch(token, 1001, 2002, &info)) << value;
    }
}

TEST_F(DownloadTicketTest, RejectsWrongUid) {
    const std::string token = issueOk(1001, 2002);

    cm::logic::DownloadTicketInfo info;
    EXPECT_EQ(cm::ErrorCode::PermissionDenied,
              m_ticket->verify(token, 1002, 2002, &info));
    EXPECT_EQ(cm::ErrorCode::PermissionDenied,
              m_ticket->touch(token, 1002, 2002, &info));
}

TEST_F(DownloadTicketTest, RejectsWrongFid) {
    const std::string token = issueOk(1001, 2002);

    cm::logic::DownloadTicketInfo info;
    EXPECT_EQ(cm::ErrorCode::PermissionDenied,
              m_ticket->verify(token, 1001, 2003, &info));
    EXPECT_EQ(cm::ErrorCode::PermissionDenied,
              m_ticket->touch(token, 1001, 2003, &info));
}

TEST_F(DownloadTicketTest, ExpiredRejected) {
    const std::string token = issueOk(1001, 2002);
    ASSERT_TRUE(overwriteTicket(*m_redis, token, 1001, 2002, nowMs() - 1000));

    cm::logic::DownloadTicketInfo info;
    EXPECT_EQ(cm::ErrorCode::PermissionDenied,
              m_ticket->verify(token, 1001, 2002, &info));
    EXPECT_EQ(cm::ErrorCode::PermissionDenied,
              m_ticket->touch(token, 1001, 2002, &info));

    std::unique_ptr<cm::cache::RedisConn> conn = m_redis->acquire();
    ASSERT_TRUE(conn);
    ASSERT_TRUE(conn->del(ticketKey(token)));
    EXPECT_EQ(cm::ErrorCode::PermissionDenied,
              m_ticket->verify(token, 1001, 2002, &info));
    EXPECT_EQ(cm::ErrorCode::PermissionDenied,
              m_ticket->touch(token, 1001, 2002, &info));
    EXPECT_EQ(-2, pttlOf(*m_redis, token));  // 失效票据不得重新创建
}

TEST_F(DownloadTicketTest, TouchSlidesWithinHardExp) {
    const std::string token = issueOk(1001, 2002);

    // 模拟 5min 快用完：TTL 只剩 5s。
    ASSERT_TRUE(shrinkTtl(*m_redis, token, 5));
    EXPECT_LE(pttlOf(*m_redis, token), 5000);

    cm::logic::DownloadTicketInfo info;
    ASSERT_EQ(cm::ErrorCode::Ok, m_ticket->touch(token, 1001, 2002, &info));
    // touch 后 TTL 回到 ~5min（仍远小于 2h 硬上限）。
    EXPECT_GT(pttlOf(*m_redis, token), 200000);
    EXPECT_LE(pttlOf(*m_redis, token), 300000);
}

TEST_F(DownloadTicketTest, HardExpCapsTouch) {
    const std::string token = issueOk(1001, 2002);
    // hard_exp 只剩 20s：续期不得超过它。
    const std::uint64_t hardExp = nowMs() + 20000;
    ASSERT_TRUE(overwriteTicket(*m_redis, token, 1001, 2002, hardExp));

    cm::logic::DownloadTicketInfo info;
    ASSERT_EQ(cm::ErrorCode::Ok, m_ticket->touch(token, 1001, 2002, &info));
    EXPECT_EQ(hardExp, info.hardExpMs);

    const long long remain = pttlOf(*m_redis, token);
    EXPECT_GT(remain, 0);
    EXPECT_LE(remain, 20000);

    // 剩余不足一秒仍可续期，不能被秒级 EXPIRE 提前拒绝。
    const std::uint64_t shortHardExp = nowMs() + 900;
    ASSERT_TRUE(overwriteTicket(*m_redis, token, 1001, 2002, shortHardExp));
    ASSERT_EQ(cm::ErrorCode::Ok, m_ticket->touch(token, 1001, 2002, &info));
    EXPECT_EQ(shortHardExp, info.hardExpMs);
    EXPECT_GT(pttlOf(*m_redis, token), 0);
    EXPECT_LE(pttlOf(*m_redis, token), 900);
}

TEST_F(DownloadTicketTest, RedisDownRejects) {
    const std::string token = issueOk(1001, 2002);

    // 关闭前仅删除本测试的 key，避免 shutdown 后无法清理的残留。
    {
        std::unique_ptr<cm::cache::RedisConn> conn = m_redis->acquire();
        ASSERT_TRUE(conn);
        ASSERT_TRUE(conn->del(ticketKey(token)));
    }
    m_redis->shutdown();
    m_down = true;

    // Redis 不可用：一律拒绝，不 fail-open。
    cm::logic::DownloadTicketInfo info;
    EXPECT_EQ(cm::ErrorCode::RedisUnavailable,
              m_ticket->verify(token, 1001, 2002, &info));
    EXPECT_EQ(cm::ErrorCode::RedisUnavailable,
              m_ticket->touch(token, 1001, 2002, &info));

    std::string fresh;
    std::uint64_t expireAtMs = 0;
    EXPECT_EQ(cm::ErrorCode::RedisUnavailable,
              m_ticket->issue(1001, 2002, &fresh, &expireAtMs));
}

}  // namespace
