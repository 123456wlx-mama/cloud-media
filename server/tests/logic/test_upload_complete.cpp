#include <gtest/gtest.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "cache/cache_store.h"
#include "cache/redis_pool.h"
#include "cm/base/md5.h"
#include "dao_fixture.h"
#include "db/file_dao.h"
#include "db/prepared_statement.h"
#include "db/transfer_dao.h"
#include "logic/upload_service.h"

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

bool ensureDir(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::string current;
    current.reserve(path.size());
    for (std::size_t i = 0; i < path.size(); ++i) {
        current.push_back(path[i]);
        if (path[i] != '/' || current.size() == 1) {
            continue;
        }
        if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
            return false;
        }
    }
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

void removeTree(const std::string& path) {
    DIR* dir = ::opendir(path.c_str());
    if (dir != NULL) {
        struct dirent* entry = NULL;
        while ((entry = ::readdir(dir)) != NULL) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") {
                continue;
            }
            removeTree(path + "/" + name);
        }
        ::closedir(dir);
    }
    ::remove(path.c_str());
}

bool writeFile(const std::string& path, const std::string& data) {
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
}

std::string readFile(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        return std::string();
    }
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

bool exists(const std::string& path) {
    return ::access(path.c_str(), F_OK) == 0;
}

class UploadCompleteServiceTest : public cm::test::DaoTestBase {
protected:
    void SetUp() override {
        cm::test::DaoTestBase::SetUp();
        if (m_pool == NULL) {
            return;  // 基类已 GTEST_SKIP（缺 MySQL）
        }
        if (!redisEnabled()) {
            GTEST_SKIP() << "set CM_TEST_REDIS=1 (db 15) to run UploadService tests";
            return;
        }

        char templ[] = "/tmp/cm_complete_XXXXXX";
        char* dir = ::mkdtemp(templ);
        ASSERT_TRUE(dir != NULL) << "mkdtemp failed";
        m_root = std::string(dir);

        std::string error;
        m_redis = cm::cache::RedisPool::create(redisOptions(), &error);
        ASSERT_TRUE(m_redis) << error;
        m_cache.reset(
            new cm::cache::CacheStore(*m_redis, cm::cache::CacheStoreOptions()));
        m_service.reset(
            new cm::logic::UploadService(*m_pool, *m_cache, m_root));
        createUser();
    }

    void TearDown() override {
        m_service.reset();
        m_cache.reset();
        if (m_redis) {
            m_redis->shutdown();
        }
        m_redis.reset();
        if (!m_root.empty()) {
            removeTree(m_root);
            m_root.clear();
        }
        // 服务层在事务内插入的 blob 不在基类的登记表里，按本次任务的 md5 显式清理；
        // 否则第二次运行会命中旧 blob 走“复用”分支，ref_count 累加、文件也不落盘。
        if (m_conn) {
            for (std::size_t i = 0; i < m_md5s.size(); ++i) {
                execPrepared("DELETE FROM t_user_file WHERE u_id = ?", m_uid);
                execPreparedString("DELETE FROM t_file WHERE f_md5 = ?", m_md5s[i]);
            }
        }
        cm::test::DaoTestBase::TearDown();
    }

    std::string createTaskForContent(const std::string& md5,
                                     std::uint64_t totalSize,
                                     std::uint32_t chunkSize,
                                     std::uint32_t totalChunks) {
        const std::string taskId = createTask(m_uid, totalChunks, totalSize);
        if (taskId.empty()) {
            return std::string();
        }
        m_md5s.push_back(md5);
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "UPDATE t_transfer_task SET tr_chunk_size = ?, tr_file_md5 = ? "
            "WHERE tr_task_id = ?");
        if (!stmt || !stmt->bindUInt64(0, chunkSize) ||
            !stmt->bindString(1, md5) || !stmt->bindString(2, taskId) ||
            !stmt->execute()) {
            return std::string();
        }
        return taskId;
    }

    // storedMd5 非空时用于模拟“文件内容与 c_md5 不一致”的坏片。
    bool addChunk(const std::string& taskId,
                  std::uint32_t index,
                  const std::string& data,
                  std::uint32_t chunkSize,
                  const std::string& storedMd5 = std::string()) {
        const std::string dir = tmpDir(taskId);
        if (!ensureDir(dir)) {
            return false;
        }
        char name[32];
        std::snprintf(name, sizeof(name), "%u.bin", index);
        if (!writeFile(dir + "/" + name, data)) {
            return false;
        }

        cm::db::TransferDao dao(*m_conn);
        cm::db::ChunkWrite chunk;
        chunk.index = index;
        chunk.offset = static_cast<std::uint64_t>(index) * chunkSize;
        chunk.size = static_cast<std::uint32_t>(data.size());
        chunk.md5 = storedMd5.empty() ? cm::md5Hex(data) : storedMd5;
        chunk.path = std::string("tmp/") + taskId + "/" + name;

        cm::db::ChunkUpsertResult result;
        return dao.upsertChunkIfAbsent(m_uid, taskId, chunk, &result) ==
                   cm::ErrorCode::Ok &&
               result.kind == cm::db::ChunkWriteKind::Inserted;
    }

    cm::protocol::UploadCompleteRq makeRequest(const std::string& taskId) {
        cm::protocol::UploadCompleteRq rq;
        rq.taskId = taskId;
        return rq;
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

    cm::db::TransferTaskRow loadTask(const std::string& taskId) {
        cm::db::TransferTaskRow row;
        cm::db::TransferDao dao(*m_conn);
        EXPECT_EQ(cm::ErrorCode::Ok, dao.findTaskByTaskId(taskId, &row));
        return row;
    }

    cm::db::BlobRow loadBlob(const std::string& md5) {
        cm::db::BlobRow blob;
        cm::db::FileDao dao(*m_conn);
        EXPECT_EQ(cm::ErrorCode::Ok, dao.findBlobByMd5(md5, &blob));
        return blob;
    }

    std::uint64_t countNodes(std::uint64_t fid) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "SELECT COUNT(*) FROM t_user_file WHERE u_id = ? AND f_id = ? "
            "AND uf_deleted_at = '1970-01-01 00:00:00.000000'");
        if (!stmt || !stmt->bindUInt64(0, m_uid) || !stmt->bindUInt64(1, fid) ||
            !stmt->execute() ||
            stmt->fetchRow() != cm::db::PreparedStatement::FetchStatus::Row) {
            return 0;
        }
        return stmt->getUInt64(0);
    }

    std::string tmpDir(const std::string& taskId) const {
        return m_root + "/tmp/" + taskId;
    }

    std::string blobPath(const std::string& md5) const {
        return m_root + "/blobs/" + md5.substr(0, 2) + "/" +
               md5.substr(2, 2) + "/" + md5;
    }

    std::string m_root;
    std::vector<std::string> m_md5s;
    std::shared_ptr<cm::cache::RedisPool> m_redis;
    std::unique_ptr<cm::cache::CacheStore> m_cache;
    std::unique_ptr<cm::logic::UploadService> m_service;
};

TEST_F(UploadCompleteServiceTest, UploadCompleteMergesAndStores) {
    const std::string part0(1024, 'a');
    const std::string part1(1024, 'b');
    const std::string part2(452, 'c');
    const std::string content = part0 + part1 + part2;
    const std::string md5 = cm::md5Hex(content);

    const std::string taskId =
        createTaskForContent(md5, content.size(), 1024, 3);
    ASSERT_FALSE(taskId.empty());
    ASSERT_TRUE(addChunk(taskId, 0, part0, 1024));
    ASSERT_TRUE(addChunk(taskId, 1, part1, 1024));
    ASSERT_TRUE(addChunk(taskId, 2, part2, 1024));

    cm::protocol::UploadCompleteRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->completeUpload(m_uid, makeRequest(taskId), &rs));

    EXPECT_EQ(0u, rs.code);
    EXPECT_GT(rs.fileId, std::uint64_t(0));
    EXPECT_GT(rs.ufId, std::uint64_t(0));
    EXPECT_EQ(std::string("phase5.bin"), rs.fileName);
    EXPECT_EQ(md5, rs.fileMd5);
    EXPECT_EQ(static_cast<std::uint64_t>(content.size()), rs.size);
    EXPECT_TRUE(rs.badIndexes.empty());

    EXPECT_TRUE(exists(blobPath(md5)));
    EXPECT_EQ(content, readFile(blobPath(md5)));

    const std::string meta = readFile(blobPath(md5) + ".meta");
    EXPECT_NE(std::string::npos, meta.find("size=" +
                                           std::to_string(content.size())));
    EXPECT_NE(std::string::npos, meta.find("md5=" + md5));

    const cm::db::BlobRow blob = loadBlob(md5);
    EXPECT_EQ(std::uint32_t(1), blob.refCount);
    EXPECT_EQ(std::uint8_t(1), blob.state);
    EXPECT_EQ(md5.substr(0, 2) + "/" + md5.substr(2, 2) + "/" + md5, blob.path);

    cm::db::FileDao fileDao(*m_conn);
    cm::db::UserFileRow node;
    ASSERT_EQ(cm::ErrorCode::Ok, fileDao.findUserFile(m_uid, rs.ufId, &node));
    EXPECT_EQ(blob.fid, node.fid);
    EXPECT_EQ(std::string("phase5.bin"), node.name);

    const cm::db::TransferTaskRow task = loadTask(taskId);
    EXPECT_EQ(4, static_cast<int>(task.status));
    EXPECT_EQ(3u, task.doneChunks);
    EXPECT_EQ(static_cast<std::uint64_t>(content.size()), task.offset);
}

TEST_F(UploadCompleteServiceTest, UploadCompleteIdempotent) {
    const std::string content(2048, 'x');
    const std::string md5 = cm::md5Hex(content);
    const std::string taskId =
        createTaskForContent(md5, content.size(), 1024, 2);
    ASSERT_FALSE(taskId.empty());
    ASSERT_TRUE(addChunk(taskId, 0, content.substr(0, 1024), 1024));
    ASSERT_TRUE(addChunk(taskId, 1, content.substr(1024), 1024));

    cm::protocol::UploadCompleteRs first;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->completeUpload(m_uid, makeRequest(taskId), &first));
    ASSERT_EQ(0u, first.code);

    cm::protocol::UploadCompleteRs second;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->completeUpload(m_uid, makeRequest(taskId), &second));
    EXPECT_EQ(0u, second.code);
    EXPECT_EQ(first.fileId, second.fileId);
    EXPECT_EQ(first.ufId, second.ufId);
    EXPECT_EQ(first.fileName, second.fileName);

    // 不重复合并、不重复加计数。
    EXPECT_EQ(std::uint32_t(1), loadBlob(md5).refCount);
    EXPECT_EQ(std::uint64_t(1), countNodes(first.fileId));
}

TEST_F(UploadCompleteServiceTest, UploadCompleteBadChunkReportsIndexes) {
    const std::string good0(1024, 'a');
    const std::string good1(1024, 'b');
    const std::string good2(452, 'c');
    const std::string md5 = cm::md5Hex(good0 + good1 + good2);

    const std::string taskId =
        createTaskForContent(md5, good0.size() + good1.size() + good2.size(),
                             1024, 3);
    ASSERT_FALSE(taskId.empty());
    ASSERT_TRUE(addChunk(taskId, 0, good0, 1024));
    // 文件内容与 c_md5 不一致：索引 1 是坏片。
    ASSERT_TRUE(addChunk(taskId, 1, good1, 1024, cm::md5Hex(good0)));
    ASSERT_TRUE(addChunk(taskId, 2, good2, 1024));

    cm::protocol::UploadCompleteRs rs;
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              m_service->completeUpload(m_uid, makeRequest(taskId), &rs));
    EXPECT_EQ(1001u, rs.code);
    ASSERT_EQ(std::size_t(1), rs.badIndexes.size());
    EXPECT_EQ(1u, rs.badIndexes[0]);

    EXPECT_EQ(1, static_cast<int>(loadTask(taskId).status));
    EXPECT_FALSE(exists(blobPath(md5)));
    EXPECT_TRUE(exists(tmpDir(taskId)));
}

TEST_F(UploadCompleteServiceTest, UploadCompleteWholeMd5Mismatch) {
    const std::string part0(1024, 'a');
    const std::string part1(1024, 'b');
    const std::string content = part0 + part1;

    // 声明一个与真实内容不同的整文件 MD5。
    const std::string declared = cm::md5Hex(std::string("not-the-content"));
    const std::string taskId =
        createTaskForContent(declared, content.size(), 1024, 2);
    ASSERT_FALSE(taskId.empty());
    ASSERT_TRUE(addChunk(taskId, 0, part0, 1024));
    ASSERT_TRUE(addChunk(taskId, 1, part1, 1024));

    cm::protocol::UploadCompleteRs rs;
    EXPECT_EQ(cm::ErrorCode::Md5Mismatch,
              m_service->completeUpload(m_uid, makeRequest(taskId), &rs));
    EXPECT_EQ(3003u, rs.code);
    EXPECT_TRUE(rs.badIndexes.empty());

    EXPECT_EQ(1, static_cast<int>(loadTask(taskId).status));
    EXPECT_FALSE(exists(blobPath(declared)));
    EXPECT_TRUE(exists(tmpDir(taskId)));
}

TEST_F(UploadCompleteServiceTest, UploadCompleteElectConflictReturns3004) {
    const std::string content(1024, 'q');
    const std::string md5 = cm::md5Hex(content);
    const std::string taskId = createTaskForContent(md5, content.size(), 1024, 1);
    ASSERT_FALSE(taskId.empty());
    ASSERT_TRUE(addChunk(taskId, 0, content, 1024));
    ASSERT_TRUE(setStatus(taskId, 7));

    cm::protocol::UploadCompleteRs rs;
    EXPECT_EQ(cm::ErrorCode::TaskNotFound,
              m_service->completeUpload(m_uid, makeRequest(taskId), &rs));
    EXPECT_EQ(3004u, rs.code);
    EXPECT_EQ(7, static_cast<int>(loadTask(taskId).status));

    // 不存在的任务同样 3004。
    cm::protocol::UploadCompleteRs missing;
    EXPECT_EQ(cm::ErrorCode::TaskNotFound,
              m_service->completeUpload(m_uid, makeRequest(nextHex32()),
                                        &missing));
    EXPECT_EQ(3004u, missing.code);
}

TEST_F(UploadCompleteServiceTest, UploadCompleteBlobExistsReusesRef) {
    const std::string content(2048, 'z');
    const std::string md5 = cm::md5Hex(content);

    const std::string firstTask =
        createTaskForContent(md5, content.size(), 1024, 2);
    ASSERT_FALSE(firstTask.empty());
    ASSERT_TRUE(addChunk(firstTask, 0, content.substr(0, 1024), 1024));
    ASSERT_TRUE(addChunk(firstTask, 1, content.substr(1024), 1024));

    cm::protocol::UploadCompleteRs first;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->completeUpload(m_uid, makeRequest(firstTask), &first));
    ASSERT_EQ(0u, first.code);
    ASSERT_EQ(std::uint32_t(1), loadBlob(md5).refCount);

    const std::string secondTask =
        createTaskForContent(md5, content.size(), 1024, 2);
    ASSERT_FALSE(secondTask.empty());
    ASSERT_TRUE(addChunk(secondTask, 0, content.substr(0, 1024), 1024));
    ASSERT_TRUE(addChunk(secondTask, 1, content.substr(1024), 1024));

    cm::protocol::UploadCompleteRs second;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->completeUpload(m_uid, makeRequest(secondTask), &second));
    EXPECT_EQ(0u, second.code);
    EXPECT_EQ(first.fileId, second.fileId);
    EXPECT_NE(first.ufId, second.ufId);
    // 同目录同名不同节点：自动改名。
    EXPECT_EQ(std::string("phase5 (2).bin"), second.fileName);

    EXPECT_EQ(std::uint32_t(2), loadBlob(md5).refCount);
    EXPECT_EQ(std::uint64_t(2), countNodes(first.fileId));
}

TEST_F(UploadCompleteServiceTest, UploadCompleteRenameFailureReturnsTo1) {
    const std::string content(1024, 'r');
    const std::string md5 = cm::md5Hex(content);
    const std::string taskId = createTaskForContent(md5, content.size(), 1024, 1);
    ASSERT_FALSE(taskId.empty());
    ASSERT_TRUE(addChunk(taskId, 0, content, 1024));

    // 让 <root>/blobs/<aa>/<bb> 变成一个普通文件，rename 目标路径必然 ENOTDIR。
    const std::string aaDir = m_root + "/blobs/" + md5.substr(0, 2);
    ASSERT_TRUE(ensureDir(aaDir));
    ASSERT_TRUE(writeFile(aaDir + "/" + md5.substr(2, 2), "blocker"));

    cm::protocol::UploadCompleteRs rs;
    EXPECT_EQ(cm::ErrorCode::NoSpace,
              m_service->completeUpload(m_uid, makeRequest(taskId), &rs));
    EXPECT_EQ(1005u, rs.code);
    // 失败回 1（可重试）；7 只保留给进程崩溃中断。
    EXPECT_EQ(1, static_cast<int>(loadTask(taskId).status));
    EXPECT_TRUE(exists(tmpDir(taskId)));
}

TEST_F(UploadCompleteServiceTest, UploadCompleteCleansTmpDir) {
    const std::string content(1024, 'm');
    const std::string md5 = cm::md5Hex(content);
    const std::string taskId = createTaskForContent(md5, content.size(), 1024, 1);
    ASSERT_FALSE(taskId.empty());
    ASSERT_TRUE(addChunk(taskId, 0, content, 1024));
    ASSERT_TRUE(exists(tmpDir(taskId)));

    cm::protocol::UploadCompleteRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->completeUpload(m_uid, makeRequest(taskId), &rs));
    EXPECT_EQ(0u, rs.code);
    EXPECT_FALSE(exists(tmpDir(taskId)));
    EXPECT_TRUE(exists(blobPath(md5)));
}

}  // namespace
