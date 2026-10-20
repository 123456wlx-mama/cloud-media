#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "cache/cache_store.h"
#include "cache/redis_pool.h"
#include "dao_fixture.h"
#include "db/prepared_statement.h"
#include "db/transfer_dao.h"
#include "logic/upload_service.h"

namespace {

const std::uint32_t kChunkSize = 4194304;

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

// ContinueUpload 自身不访问 Redis；Redis 只为构造 UploadService 的 CacheStore。
class ContinueUploadServiceTest : public cm::test::DaoTestBase {
protected:
    void SetUp() override {
        cm::test::DaoTestBase::SetUp();
        if (m_pool == nullptr) {
            return;  // 基类已 GTEST_SKIP（缺 MySQL）
        }
        if (!redisEnabled()) {
            GTEST_SKIP() << "set CM_TEST_REDIS=1 (db 15) to run UploadService tests";
            return;
        }

        std::string error;
        m_redis = cm::cache::RedisPool::create(redisOptions(), &error);
        ASSERT_TRUE(m_redis) << error;
        m_cache.reset(
            new cm::cache::CacheStore(*m_redis, cm::cache::CacheStoreOptions()));
        m_service.reset(new cm::logic::UploadService(*m_pool, *m_cache));
        createUser();
    }

    void TearDown() override {
        m_service.reset();
        m_cache.reset();
        if (m_redis) {
            m_redis->shutdown();
        }
        m_redis.reset();
        cm::test::DaoTestBase::TearDown();
    }

    cm::protocol::ContinueUploadRq makeRequest(const std::string& taskId) {
        cm::protocol::ContinueUploadRq rq;
        rq.taskId = taskId;
        return rq;
    }

    // 走真实 DAO 写分片行，确保 done_chunks / tr_offset 的基线由业务路径产生。
    void addChunk(const std::string& taskId,
                  std::uint32_t index,
                  std::uint32_t size) {
        cm::db::TransferDao dao(*m_conn);
        cm::db::ChunkWrite chunk;
        chunk.index = index;
        chunk.offset = static_cast<std::uint64_t>(index) * kChunkSize;
        chunk.size = size;
        chunk.md5 = nextHex32();
        char name[32];
        std::snprintf(name, sizeof(name), "%u.bin", index);
        chunk.path = std::string("tmp/") + taskId + "/" + name;

        cm::db::ChunkUpsertResult result;
        ASSERT_EQ(cm::ErrorCode::Ok,
                  dao.upsertChunkIfAbsent(m_uid, taskId, chunk, &result));
        ASSERT_EQ(cm::db::ChunkWriteKind::Inserted, result.kind);
    }

    bool setStatus(const std::string& taskId, std::uint8_t status) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "UPDATE t_transfer_task SET tr_status = ? WHERE tr_task_id = ?");
        if (!stmt || !stmt->bindUInt64(0, status) ||
            !stmt->bindString(1, taskId)) {
            return false;
        }
        return stmt->execute();
    }

    bool setExpirePast(const std::string& taskId) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "UPDATE t_transfer_task "
            "SET tr_expire_at = DATE_SUB(NOW(6), INTERVAL 1 HOUR) "
            "WHERE tr_task_id = ?");
        if (!stmt || !stmt->bindString(0, taskId)) {
            return false;
        }
        return stmt->execute();
    }

    bool setCounters(const std::string& taskId,
                     std::uint32_t doneChunks,
                     std::uint64_t doneBytes) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "UPDATE t_transfer_task SET tr_done_chunks = ?, tr_offset = ? "
            "WHERE tr_task_id = ?");
        if (!stmt || !stmt->bindUInt64(0, doneChunks) ||
            !stmt->bindUInt64(1, doneBytes) || !stmt->bindString(2, taskId)) {
            return false;
        }
        return stmt->execute();
    }

    cm::db::TransferTaskRow loadTask(const std::string& taskId) {
        cm::db::TransferTaskRow row;
        cm::db::TransferDao dao(*m_conn);
        EXPECT_EQ(cm::ErrorCode::Ok, dao.findTaskByTaskId(taskId, &row));
        return row;
    }

    std::shared_ptr<cm::cache::RedisPool> m_redis;
    std::unique_ptr<cm::cache::CacheStore> m_cache;
    std::unique_ptr<cm::logic::UploadService> m_service;
};

TEST_F(ContinueUploadServiceTest, ContinueUploadEmptyBitmap) {
    const std::string taskId = createTask(m_uid, 13, 50ULL * 1024 * 1024);
    ASSERT_FALSE(taskId.empty());

    cm::protocol::ContinueUploadRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->continueUpload(m_uid, makeRequest(taskId), &rs));

    EXPECT_EQ(0u, rs.code);
    EXPECT_EQ(taskId, rs.taskId);
    EXPECT_EQ(0, static_cast<int>(rs.status));
    EXPECT_EQ(kChunkSize, rs.chunkSize);
    EXPECT_EQ(13u, rs.totalChunks);
    EXPECT_EQ(0u, rs.doneChunks);
    EXPECT_EQ(std::uint64_t(0), rs.doneBytes);
    // 定长：ceil(13/8)=2，空任务也必须是 2 个 0 字节，不能是空串。
    ASSERT_EQ(std::size_t(2), rs.bitmap.size());
    EXPECT_EQ(std::string(2, '\0'), rs.bitmap);
    EXPECT_GT(rs.expireAtMs, std::uint64_t(0));
}

TEST_F(ContinueUploadServiceTest, ContinueUploadPartialBitmap) {
    const std::string taskId = createTask(m_uid, 13, 50ULL * 1024 * 1024);
    ASSERT_FALSE(taskId.empty());
    for (std::uint32_t index = 0; index < 6; ++index) {
        addChunk(taskId, index, kChunkSize);
    }

    cm::protocol::ContinueUploadRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->continueUpload(m_uid, makeRequest(taskId), &rs));

    EXPECT_EQ(0u, rs.code);
    EXPECT_EQ(6u, rs.doneChunks);
    EXPECT_EQ(std::uint64_t(6) * kChunkSize, rs.doneBytes);
    ASSERT_EQ(std::size_t(2), rs.bitmap.size());
    // 前 6 片 → byte0 = 0b00111111，其余 0。
    EXPECT_EQ(0x3F, static_cast<unsigned char>(rs.bitmap[0]));
    EXPECT_EQ(0x00, static_cast<unsigned char>(rs.bitmap[1]));

    // 跨字节：第 8 片 → byte1 的 bit0（LSB-first）。
    addChunk(taskId, 8, kChunkSize);
    cm::protocol::ContinueUploadRs crossByte;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->continueUpload(m_uid, makeRequest(taskId), &crossByte));
    EXPECT_EQ(7u, crossByte.doneChunks);
    EXPECT_EQ(0x3F, static_cast<unsigned char>(crossByte.bitmap[0]));
    EXPECT_EQ(0x01, static_cast<unsigned char>(crossByte.bitmap[1]));
}

TEST_F(ContinueUploadServiceTest, ContinueUploadCompleteBitmap) {
    const std::string taskId = createTask(m_uid, 3, 3ULL * kChunkSize);
    ASSERT_FALSE(taskId.empty());
    addChunk(taskId, 0, kChunkSize);
    addChunk(taskId, 1, kChunkSize);
    addChunk(taskId, 2, kChunkSize);

    cm::protocol::ContinueUploadRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->continueUpload(m_uid, makeRequest(taskId), &rs));

    EXPECT_EQ(0u, rs.code);
    EXPECT_EQ(3u, rs.doneChunks);
    EXPECT_EQ(std::uint64_t(3) * kChunkSize, rs.doneBytes);
    ASSERT_EQ(std::size_t(1), rs.bitmap.size());
    EXPECT_EQ(0x07, static_cast<unsigned char>(rs.bitmap[0]));
}

TEST_F(ContinueUploadServiceTest, ContinueUploadFixesCounterDrift) {
    const std::string taskId = createTask(m_uid, 13, 50ULL * 1024 * 1024);
    ASSERT_FALSE(taskId.empty());
    addChunk(taskId, 0, kChunkSize);
    addChunk(taskId, 1, kChunkSize);

    // 模拟崩溃窗口留下的计数缓存漂移。
    ASSERT_TRUE(setCounters(taskId, 0, 0));

    cm::protocol::ContinueUploadRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->continueUpload(m_uid, makeRequest(taskId), &rs));

    EXPECT_EQ(0u, rs.code);
    EXPECT_EQ(2u, rs.doneChunks);
    EXPECT_EQ(std::uint64_t(2) * kChunkSize, rs.doneBytes);

    const cm::db::TransferTaskRow row = loadTask(taskId);
    EXPECT_EQ(2u, row.doneChunks);
    EXPECT_EQ(std::uint64_t(2) * kChunkSize, row.offset);
}

TEST_F(ContinueUploadServiceTest, ContinueUploadExpired) {
    const std::string expiredByTime =
        createTask(m_uid, 4, 4ULL * kChunkSize);
    ASSERT_FALSE(expiredByTime.empty());
    ASSERT_TRUE(setExpirePast(expiredByTime));

    cm::protocol::ContinueUploadRs rs;
    EXPECT_EQ(cm::ErrorCode::TaskExpired,
              m_service->continueUpload(m_uid, makeRequest(expiredByTime), &rs));
    EXPECT_EQ(3007u, rs.code);
    EXPECT_TRUE(rs.bitmap.empty());

    const std::string expiredByStatus =
        createTask(m_uid, 4, 4ULL * kChunkSize);
    ASSERT_FALSE(expiredByStatus.empty());
    ASSERT_TRUE(setStatus(expiredByStatus, 6));

    cm::protocol::ContinueUploadRs statusRs;
    EXPECT_EQ(cm::ErrorCode::TaskExpired,
              m_service->continueUpload(m_uid, makeRequest(expiredByStatus),
                                        &statusRs));
    EXPECT_EQ(3007u, statusRs.code);
}

TEST_F(ContinueUploadServiceTest, ContinueUploadTaskOfOtherUser) {
    const std::uint64_t owner = m_uid;
    const std::string taskId = createTask(owner, 4, 4ULL * kChunkSize);
    ASSERT_FALSE(taskId.empty());

    const std::uint64_t other = createUser();
    ASSERT_NE(owner, other);

    cm::protocol::ContinueUploadRs rs;
    EXPECT_EQ(cm::ErrorCode::PermissionDenied,
              m_service->continueUpload(other, makeRequest(taskId), &rs));
    EXPECT_EQ(1004u, rs.code);

    // 不存在的任务同样 1004，不泄露存在性。
    cm::protocol::ContinueUploadRs missing;
    EXPECT_EQ(cm::ErrorCode::PermissionDenied,
              m_service->continueUpload(owner, makeRequest(nextHex32()),
                                        &missing));
    EXPECT_EQ(1004u, missing.code);
}

TEST_F(ContinueUploadServiceTest, ContinueUploadRejectsNonWritableStates) {
    const std::uint8_t states[] = {3, 4, 5, 7};
    for (std::size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        const std::string taskId = createTask(m_uid, 2, 2ULL * kChunkSize);
        ASSERT_FALSE(taskId.empty());
        ASSERT_TRUE(setStatus(taskId, states[i]));

        cm::protocol::ContinueUploadRs rs;
        EXPECT_EQ(cm::ErrorCode::TaskStateInvalid,
                  m_service->continueUpload(m_uid, makeRequest(taskId), &rs))
            << "status=" << static_cast<int>(states[i]);
        EXPECT_EQ(3005u, rs.code);
    }
}

TEST_F(ContinueUploadServiceTest, ContinueUploadRejectsOversizedBitmap) {
    const std::uint32_t oversized = cm::protocol::kMaxChunkCount + 1;
    const std::string taskId =
        createTask(m_uid, oversized,
                   static_cast<std::uint64_t>(oversized) * kChunkSize);
    ASSERT_FALSE(taskId.empty());

    cm::protocol::ContinueUploadRs rs;
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              m_service->continueUpload(m_uid, makeRequest(taskId), &rs));
    EXPECT_EQ(1001u, rs.code);
    EXPECT_TRUE(rs.bitmap.empty());

    // 非法 task_id 也按参数错误处理，不查库。
    cm::protocol::ContinueUploadRs bad;
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              m_service->continueUpload(m_uid, makeRequest("not-a-task-id"),
                                        &bad));
    EXPECT_EQ(1001u, bad.code);
}

}  // namespace
