#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

#include "cache/cache_store.h"
#include "cache/redis_pool.h"
#include "db/file_dao.h"
#include "db/prepared_statement.h"
#include "db/transfer_dao.h"
#include "logic/upload_service.h"
#include "dao_fixture.h"

namespace {

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

class UploadInitServiceTest : public cm::test::DaoTestBase {
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
        m_cache.reset(new cm::cache::CacheStore(*m_redis, cm::cache::CacheStoreOptions()));
        m_service.reset(new cm::logic::UploadService(*m_pool, *m_cache));
        createUser();
    }

    void TearDown() override {
        m_service.reset();
        m_cache.reset();
        if (m_redis && !m_redisDown) {
            m_redis->shutdown();
        }
        m_redis.reset();
        cm::test::DaoTestBase::TearDown();
    }

    cm::protocol::UploadInitRq makeRequest(const std::string& md5,
                                          std::uint64_t size,
                                          const std::string& name,
                                          std::uint64_t parentId,
                                          std::uint32_t reqId) {
        cm::protocol::UploadInitRq rq;
        rq.parentId = parentId;
        rq.fileName = name;
        rq.totalSize = size;
        rq.fileMd5 = md5;
        rq.chunkSize = 4194304;
        rq.totalChunks = 0;
        rq.flags = 1;
        rq.reqId = reqId;
        return rq;
    }

    std::uint64_t countTasks(std::uint64_t uid) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "SELECT COUNT(*) FROM t_transfer_task WHERE tr_user_id = ?");
        if (!stmt || !stmt->bindUInt64(0, uid) || !stmt->execute()) {
            return 0;
        }
        if (stmt->fetchRow() != cm::db::PreparedStatement::FetchStatus::Row) {
            return 0;
        }
        return stmt->getUInt64(0);
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
        cm::db::BlobRow row;
        cm::db::FileDao dao(*m_conn);
        EXPECT_EQ(cm::ErrorCode::Ok, dao.getBlobById(fid, &row));
        return row;
    }

    std::shared_ptr<cm::cache::RedisPool> m_redis;
    std::unique_ptr<cm::cache::CacheStore> m_cache;
    std::unique_ptr<cm::logic::UploadService> m_service;
    bool m_redisDown = false;
};

TEST_F(UploadInitServiceTest, UploadInitCreatesTask) {
    const std::string md5 = nextHex32();  // 全新内容，不会秒传命中
    const std::uint64_t size = 50ULL * 1024 * 1024;

    cm::protocol::UploadInitRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->init(m_uid, makeRequest(md5, size, "phase5.bin", 0, 1), &rs));

    EXPECT_EQ(0u, rs.code);
    EXPECT_EQ(0, static_cast<int>(rs.result));
    EXPECT_EQ(std::size_t(32), rs.taskId.size());
    EXPECT_EQ(4194304u, rs.chunkSize);
    EXPECT_EQ(13u, rs.totalChunks);
    EXPECT_EQ(std::string("phase5.bin"), rs.fileName);
    EXPECT_EQ(std::uint64_t(0), rs.ufId);
    EXPECT_GT(rs.expireAtMs, std::uint64_t(0));
    trackTask(rs.taskId);

    cm::db::TransferDao dao(*m_conn);
    cm::db::TransferTaskRow row;
    ASSERT_EQ(cm::ErrorCode::Ok, dao.findTaskByTaskId(rs.taskId, &row));
    EXPECT_EQ(m_uid, row.userId);
    EXPECT_EQ(0, static_cast<int>(row.status));
    EXPECT_EQ(1, static_cast<int>(row.type));
    EXPECT_EQ(size, row.totalSize);
    EXPECT_EQ(13u, row.totalChunks);
    EXPECT_EQ(md5, row.fileMd5);
    EXPECT_EQ(std::uint64_t(0), row.parentUfId);
}

TEST_F(UploadInitServiceTest, UploadInitInstantHitCreatesNode) {
    const std::uint64_t fid = createBlob(4096);
    ASSERT_GT(fid, std::uint64_t(0));
    const cm::db::BlobRow blob = loadBlob(fid);
    ASSERT_EQ(std::uint8_t(1), blob.state);

    cm::protocol::UploadInitRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->init(m_uid,
                              makeRequest(blob.md5, blob.size, "hit.bin", 0, 2),
                              &rs));

    EXPECT_EQ(0u, rs.code);
    EXPECT_EQ(1, static_cast<int>(rs.result));
    EXPECT_EQ(fid, rs.fileId);
    ASSERT_GT(rs.ufId, std::uint64_t(0));
    EXPECT_EQ(std::string("hit.bin"), rs.fileName);
    EXPECT_EQ(std::uint64_t(0), rs.expireAtMs);
    EXPECT_TRUE(rs.taskId.empty());
    trackNode(rs.ufId);

    const cm::db::BlobRow after = loadBlob(fid);
    EXPECT_EQ(blob.refCount + 1, after.refCount);

    cm::db::FileDao fileDao(*m_conn);
    cm::db::UserFileRow node;
    ASSERT_EQ(cm::ErrorCode::Ok, fileDao.findUserFile(m_uid, rs.ufId, &node));
    EXPECT_EQ(std::string("hit.bin"), node.name);
    EXPECT_EQ(fid, node.fid);
    EXPECT_FALSE(node.isDir);
}

TEST_F(UploadInitServiceTest, UploadInitReqIdIdempotent) {
    const std::string md5 = nextHex32();
    const cm::protocol::UploadInitRq rq = makeRequest(md5, 8192, "idem.bin", 0, 7);

    cm::protocol::UploadInitRs first;
    ASSERT_EQ(cm::ErrorCode::Ok, m_service->init(m_uid, rq, &first));
    ASSERT_EQ(0u, first.code);
    trackTask(first.taskId);

    // 同一 req_id 重放：必须回放同一 task_id，不产生第二条任务。
    cm::protocol::UploadInitRs second;
    ASSERT_EQ(cm::ErrorCode::Ok, m_service->init(m_uid, rq, &second));
    EXPECT_EQ(0u, second.code);
    EXPECT_EQ(first.taskId, second.taskId);
    EXPECT_EQ(std::uint64_t(1), countTasks(m_uid));

    // 换一个 req_id 是新的用户意图，允许再建一条任务。
    cm::protocol::UploadInitRs third;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->init(m_uid,
                              makeRequest(md5, 8192, "idem.bin", 0, 8), &third));
    EXPECT_NE(first.taskId, third.taskId);
    EXPECT_EQ(std::uint64_t(2), countTasks(m_uid));
    trackTask(third.taskId);
}

TEST_F(UploadInitServiceTest, UploadInitAutoRename) {
    const std::uint64_t firstFid = createBlob(1024);
    const std::uint64_t secondFid = createBlob(2048);
    const cm::db::BlobRow firstBlob = loadBlob(firstFid);
    const cm::db::BlobRow secondBlob = loadBlob(secondFid);

    cm::protocol::UploadInitRs rs1;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->init(m_uid,
                              makeRequest(firstBlob.md5, firstBlob.size,
                                          "same.bin", 0, 11),
                              &rs1));
    ASSERT_EQ(1, static_cast<int>(rs1.result));
    EXPECT_EQ(std::string("same.bin"), rs1.fileName);
    trackNode(rs1.ufId);

    cm::protocol::UploadInitRs rs2;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->init(m_uid,
                              makeRequest(secondBlob.md5, secondBlob.size,
                                          "same.bin", 0, 12),
                              &rs2));
    ASSERT_EQ(1, static_cast<int>(rs2.result));
    EXPECT_EQ(std::string("same (2).bin"), rs2.fileName);
    EXPECT_NE(rs1.ufId, rs2.ufId);
    trackNode(rs2.ufId);
}

TEST_F(UploadInitServiceTest, UploadInitRejectsBadParent) {
    const std::string md5 = nextHex32();

    // 不存在的 parent。
    cm::protocol::UploadInitRs rs;
    EXPECT_EQ(cm::ErrorCode::NotFound,
              m_service->init(m_uid,
                              makeRequest(md5, 8192, "a.bin", 999999999ULL, 21),
                              &rs));
    EXPECT_EQ(1002u, rs.code);

    // parent 是文件而不是目录。
    const std::uint64_t fid = createBlob(1024);
    const std::uint64_t fileNode = createNode(m_uid, "file.bin", fid);
    ASSERT_GT(fileNode, std::uint64_t(0));
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              m_service->init(m_uid,
                              makeRequest(md5, 8192, "a.bin", fileNode, 22),
                              &rs));
    EXPECT_EQ(1001u, rs.code);

    // parent 已被软删：按不存在处理，不泄露存在性。
    const std::uint64_t dirNode = createNode(m_uid, "gone_dir", 0);
    ASSERT_GT(dirNode, std::uint64_t(0));
    bool already = false;
    cm::db::FileDao fileDao(*m_conn);
    ASSERT_EQ(cm::ErrorCode::Ok, fileDao.softDelete(m_uid, dirNode, &already));

    EXPECT_EQ(cm::ErrorCode::NotFound,
              m_service->init(m_uid,
                              makeRequest(md5, 8192, "a.bin", dirNode, 23), &rs));
    EXPECT_EQ(1002u, rs.code);

    // 三次失败都不应留下任务。
    EXPECT_EQ(std::uint64_t(0), countTasks(m_uid));
}

TEST_F(UploadInitServiceTest, UploadInitIgnoresBlobStateNotReady) {
    const std::uint64_t fid = createBlob(4096);
    ASSERT_GT(fid, std::uint64_t(0));
    ASSERT_TRUE(setBlobState(fid, 2));  // 孤儿 blob，不可秒传

    const cm::db::BlobRow blob = loadBlob(fid);
    ASSERT_EQ(std::uint8_t(2), blob.state);

    cm::protocol::UploadInitRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->init(m_uid,
                              makeRequest(blob.md5, blob.size, "orphan.bin", 0, 31),
                              &rs));

    EXPECT_EQ(0u, rs.code);
    EXPECT_EQ(0, static_cast<int>(rs.result));  // 未命中，走上传
    EXPECT_FALSE(rs.taskId.empty());
    EXPECT_EQ(std::uint64_t(0), rs.ufId);
    trackTask(rs.taskId);

    // 计数未被改动。
    EXPECT_EQ(blob.refCount, loadBlob(fid).refCount);
}

TEST_F(UploadInitServiceTest, UploadInitRedisDownFallsBackToMysql) {
    const std::uint64_t fid = createBlob(4096);
    const cm::db::BlobRow blob = loadBlob(fid);

    m_redis->shutdown();
    m_redisDown = true;

    // 秒传：Redis 不可用时回源 MySQL，仍然命中。
    cm::protocol::UploadInitRs hit;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->init(m_uid,
                              makeRequest(blob.md5, blob.size, "fallback.bin", 0, 41),
                              &hit));
    EXPECT_EQ(0u, hit.code);
    EXPECT_EQ(1, static_cast<int>(hit.result));
    EXPECT_EQ(fid, hit.fileId);
    trackNode(hit.ufId);

    // 未命中：建任务同样不受影响。
    cm::protocol::UploadInitRs created;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->init(m_uid,
                              makeRequest(nextHex32(), 8192, "fallback2.bin", 0, 42),
                              &created));
    EXPECT_EQ(0u, created.code);
    EXPECT_EQ(0, static_cast<int>(created.result));
    EXPECT_FALSE(created.taskId.empty());
    trackTask(created.taskId);
}

}  // namespace
