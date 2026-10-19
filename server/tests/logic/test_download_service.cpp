#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cache/cache_store.h"
#include "cache/redis_pool.h"
#include "cm/base/thread_pool.h"
#include "cm/protocol/message_type.h"
#include "cm/protocol/messages/download.h"
#include "dao_fixture.h"
#include "db/file_dao.h"
#include "db/prepared_statement.h"
#include "logic/block_sender.h"
#include "logic/download_handlers.h"
#include "logic/download_service.h"
#include "logic/download_ticket.h"
#include "logic/upload_fs.h"

namespace {

const std::uint32_t kChunkSize = 4194304;

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

class DownloadServiceTest : public cm::test::DaoTestBase {
protected:
    void SetUp() override {
        cm::test::DaoTestBase::SetUp();
        if (m_pool == NULL) {
            return;  // 基类已 GTEST_SKIP（缺 MySQL）
        }
        if (!redisEnabled()) {
            GTEST_SKIP() << "set CM_TEST_REDIS=1 (db 15) to run DownloadService tests";
            return;
        }

        char templ[] = "/tmp/cm_dlservice_XXXXXX";
        char* dir = ::mkdtemp(templ);
        ASSERT_TRUE(dir != NULL) << "mkdtemp failed";
        m_root = std::string(dir);

        std::string error;
        m_redis = cm::cache::RedisPool::create(redisOptions(), &error);
        ASSERT_TRUE(m_redis) << error;
        m_ticket.reset(new cm::logic::DownloadTicket(*m_redis));
        m_service.reset(new cm::logic::DownloadService(*m_pool, *m_redis, m_root));
        createUser();
    }

    void TearDown() override {
        m_service.reset();
        m_ticket.reset();
        if (m_redis) {
            // 先归还连接再 shutdown，否则每个用例硬等 5 秒。
            {
                std::unique_ptr<cm::cache::RedisConn> conn = m_redis->acquire();
                if (conn) {
                    for (std::size_t i = 0; i < m_tokens.size(); ++i) {
                        conn->del(cm::cache::CacheKey::downloadTicket(m_tokens[i]));
                        conn->del(cm::cache::CacheKey::downloadDone(m_uid, m_tokens[i]));
                    }
                }
            }
            m_redis->shutdown();
        }
        m_redis.reset();
        if (!m_root.empty()) {
            cm::logic::removeTree(m_root);
            m_root.clear();
        }
        cm::test::DaoTestBase::TearDown();
    }

    std::string issueTicket(std::uint64_t uid, std::uint64_t fid) {
        std::string token;
        std::uint64_t expireAtMs = 0;
        EXPECT_EQ(cm::ErrorCode::Ok,
                  m_ticket->issue(uid, fid, &token, &expireAtMs));
        EXPECT_EQ(std::size_t(64), token.size());
        m_tokens.push_back(token);
        return token;
    }

    // 直接改写 ticket 的 hard_exp（模拟过期）。
    void overwriteTicket(const std::string& token, std::uint64_t uid,
                         std::uint64_t fid, std::uint64_t hardExpMs) {
        std::unique_ptr<cm::cache::RedisConn> conn = m_redis->acquire();
        ASSERT_TRUE(conn);
        const std::string value = std::to_string(uid) + "|" +
                                  std::to_string(fid) + "|" +
                                  std::to_string(hardExpMs) + "|0";
        ASSERT_TRUE(conn->setPx(cm::cache::CacheKey::downloadTicket(token), value,
                                std::chrono::milliseconds(300000)));
    }

    bool setBlobState(std::uint64_t fid, std::uint8_t state) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "UPDATE t_file SET f_state = ? WHERE f_id = ?");
        if (!stmt || !stmt->bindUInt64(0, state) || !stmt->bindUInt64(1, fid)) {
            return false;
        }
        return stmt->execute();
    }

    cm::db::BlobRow loadBlob(std::uint64_t fid) {
        cm::db::BlobRow blob;
        cm::db::FileDao dao(*m_conn);
        EXPECT_EQ(cm::ErrorCode::Ok, dao.getBlobById(fid, &blob));
        return blob;
    }

    std::string m_root;
    std::shared_ptr<cm::cache::RedisPool> m_redis;
    std::unique_ptr<cm::logic::DownloadTicket> m_ticket;
    std::unique_ptr<cm::logic::DownloadService> m_service;
    std::vector<std::string> m_tokens;
};

TEST_F(DownloadServiceTest, DownloadOpenOwnFile) {
    const std::uint64_t fid = createBlob(5000);
    const cm::db::BlobRow blob = loadBlob(fid);
    const std::uint64_t ufId = createNode(m_uid, "video.mp4", fid);
    ASSERT_GT(ufId, std::uint64_t(0));

    cm::protocol::DownloadOpenRq rq;
    rq.ufId = ufId;
    rq.startBlock = 0;
    cm::protocol::DownloadOpenRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok, m_service->openDownload(m_uid, rq, &rs));

    EXPECT_EQ(0u, rs.code);
    EXPECT_EQ(fid, rs.fId);
    EXPECT_EQ(std::uint64_t(5000), rs.size);
    EXPECT_EQ(blob.md5, rs.fileMd5);
    EXPECT_EQ(kChunkSize, rs.blockSize);
    EXPECT_EQ(1u, rs.blockCount);
    EXPECT_EQ(std::size_t(64), rs.ticket.size());
}

TEST_F(DownloadServiceTest, DownloadOpenRejectsOtherUser) {
    const std::uint64_t owner = m_uid;
    const std::uint64_t fid = createBlob(1024);
    const std::uint64_t ufId = createNode(owner, "private.bin", fid);
    const std::uint64_t other = createUser();
    ASSERT_NE(owner, other);

    cm::protocol::DownloadOpenRq rq;
    rq.ufId = ufId;
    cm::protocol::DownloadOpenRs rs;
    EXPECT_EQ(cm::ErrorCode::NotFound,
              m_service->openDownload(other, rq, &rs));
    EXPECT_EQ(1002u, rs.code);
    EXPECT_TRUE(rs.ticket.empty());
}

TEST_F(DownloadServiceTest, DownloadOpenMissingBlob) {
    const std::uint64_t fid = createBlob(1024);
    ASSERT_TRUE(setBlobState(fid, 2));  // 孤儿，不可下载
    const std::uint64_t ufId = createNode(m_uid, "orphan.bin", fid);

    cm::protocol::DownloadOpenRq rq;
    rq.ufId = ufId;
    cm::protocol::DownloadOpenRs rs;
    EXPECT_EQ(cm::ErrorCode::FileGone, m_service->openDownload(m_uid, rq, &rs));
    EXPECT_EQ(3001u, rs.code);
    EXPECT_TRUE(rs.ticket.empty());
}

TEST_F(DownloadServiceTest, DownloadStatMismatchDetected) {
    const std::uint64_t fid = createBlob(5000);
    const cm::db::BlobRow blob = loadBlob(fid);
    const std::uint64_t ufId = createNode(m_uid, "f.bin", fid);

    cm::protocol::DownloadStatRq rq;
    rq.ufId = ufId;
    cm::protocol::DownloadStatRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok, m_service->stat(m_uid, rq, &rs));

    EXPECT_EQ(0u, rs.code);
    EXPECT_EQ(fid, rs.fId);
    EXPECT_EQ(std::uint64_t(5000), rs.size);
    EXPECT_EQ(blob.md5, rs.fileMd5);
    EXPECT_EQ(kChunkSize, rs.blockSize);
    EXPECT_EQ(1u, rs.blockCount);
    EXPECT_EQ(1, static_cast<int>(rs.state));
    // 客户端 .part 若 size/md5 与此不符，即视为远端已变，应丢弃重下。
}

TEST_F(DownloadServiceTest, DownloadRangeOutOfBounds) {
    const std::uint64_t fid = createBlob(kChunkSize);  // 恰好 1 块
    const std::string ticket = issueTicket(m_uid, fid);

    cm::ThreadPool ioPool(2, "test-io");
    cm::logic::BlockSender buffered(
        *m_pool, *m_redis,
        [&ioPool](cm::logic::BlockSender::Task t) {
            return ioPool.run(std::move(t));
        },
        m_root);
    cm::logic::DownloadHandlers handlers(*m_service, buffered);

    std::uint32_t errCode = 0;
    cm::logic::DownloadHandlers::Conn conn;
    conn.sendError = [&errCode](std::uint32_t, std::uint32_t code,
                                const std::string&) { errCode = code; };
    conn.sendRaw = [](const std::string&) {};
    conn.sendFile = [](const std::string&, int, std::int64_t, std::size_t) {
        return true;
    };
    conn.sendFrame = [](std::uint16_t, std::uint32_t, const std::string&) {};

    cm::protocol::DownloadRangeRq rq;
    rq.ticket = ticket;
    rq.fId = fid;
    rq.blockIndex = 1;  // 越界
    std::string payload;
    ASSERT_TRUE(rq.encode(&payload));
    cm::protocol::Frame frame;
    frame.type = static_cast<std::uint16_t>(cm::protocol::MessageType::DownloadRangeRq);
    frame.seq = 42;
    frame.payload = payload;

    handlers.handleRange(m_uid, frame, conn);
    EXPECT_EQ(1001u, errCode);  // 越界 → ERROR_RS 1001
}

TEST_F(DownloadServiceTest, DownloadRangeTicketExpired) {
    const std::uint64_t fid = createBlob(kChunkSize);
    const std::string ticket = issueTicket(m_uid, fid);
    overwriteTicket(ticket, m_uid, fid, nowMs() - 1000);  // 过期

    cm::ThreadPool ioPool(2, "test-io");
    cm::logic::BlockSender buffered(
        *m_pool, *m_redis,
        [&ioPool](cm::logic::BlockSender::Task t) {
            return ioPool.run(std::move(t));
        },
        m_root);
    cm::logic::DownloadHandlers handlers(*m_service, buffered);

    std::uint32_t errCode = 0;
    cm::logic::DownloadHandlers::Conn conn;
    conn.sendError = [&errCode](std::uint32_t, std::uint32_t code,
                                const std::string&) { errCode = code; };
    conn.sendRaw = [](const std::string&) {};
    conn.sendFile = [](const std::string&, int, std::int64_t, std::size_t) {
        return true;
    };
    conn.sendFrame = [](std::uint16_t, std::uint32_t, const std::string&) {};

    cm::protocol::DownloadRangeRq rq;
    rq.ticket = ticket;
    rq.fId = fid;
    rq.blockIndex = 0;
    std::string payload;
    ASSERT_TRUE(rq.encode(&payload));
    cm::protocol::Frame frame;
    frame.type = static_cast<std::uint16_t>(cm::protocol::MessageType::DownloadRangeRq);
    frame.seq = 7;
    frame.payload = payload;

    handlers.handleRange(m_uid, frame, conn);
    EXPECT_EQ(1004u, errCode);  // 票据过期 → ERROR_RS 1004
}

TEST_F(DownloadServiceTest, DownloadDoneIdempotent) {
    const std::uint64_t fid = createBlob(5000);
    const cm::db::BlobRow blob = loadBlob(fid);
    const std::string ticket = issueTicket(m_uid, fid);

    cm::protocol::DownloadDoneRq rq;
    rq.ticket = ticket;
    rq.fId = fid;
    rq.size = 5000;
    rq.fileMd5 = blob.md5;
    rq.verifyOk = 1;

    cm::protocol::DownloadDoneRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok, m_service->done(m_uid, rq, &rs));
    EXPECT_EQ(0u, rs.code);
    EXPECT_EQ(1u, rs.accepted);

    // 重复上报：Redis 去重，幂等返回，不重复计数。
    cm::protocol::DownloadDoneRs rs2;
    ASSERT_EQ(cm::ErrorCode::Ok, m_service->done(m_uid, rq, &rs2));
    EXPECT_EQ(0u, rs2.code);
    EXPECT_EQ(1u, rs2.accepted);
}

TEST_F(DownloadServiceTest, DownloadDoneMd5MismatchWarns) {
    const std::uint64_t fid = createBlob(5000);
    const std::string ticket = issueTicket(m_uid, fid);

    cm::protocol::DownloadDoneRq rq;
    rq.ticket = ticket;
    rq.fId = fid;
    rq.size = 5000;
    rq.fileMd5 = "00000000000000000000000000000000";  // 与远端不符
    rq.verifyOk = 0;

    cm::protocol::DownloadDoneRs rs;
    EXPECT_EQ(cm::ErrorCode::Md5Mismatch, m_service->done(m_uid, rq, &rs));
    EXPECT_EQ(3003u, rs.code);  // 仅告警
    EXPECT_EQ(0u, rs.accepted);
}

}  // namespace
