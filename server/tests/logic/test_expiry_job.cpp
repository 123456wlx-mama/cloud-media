#include <gtest/gtest.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "EventLoop.h"
#include "TimerQueue.h"
#include "cm/base/thread_pool.h"
#include "cm/config/server_config.h"
#include "dao_fixture.h"
#include "db/prepared_statement.h"
#include "db/transfer_dao.h"
#include "logic/expiry_job.h"
#include "logic/upload_fs.h"
#include "app/server_app.h"

namespace {

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

bool writeFile(const std::string& path, const std::string& data) {
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
}

bool exists(const std::string& path) {
    return ::access(path.c_str(), F_OK) == 0;
}

void makeStale(const std::string& path) {
    const std::time_t now = std::time(nullptr);
    struct utimbuf times;
    times.actime = now - 7200;
    times.modtime = now - 7200;
    ::utime(path.c_str(), &times);
}

class ExpiryJobTest : public cm::test::DaoTestBase {
protected:
    void SetUp() override {
        cm::test::DaoTestBase::SetUp();
        if (m_pool == nullptr) {
            return;  // 基类已 GTEST_SKIP（缺 MySQL）
        }

        char templ[] = "/tmp/cm_expiry_XXXXXX";
        char* dir = ::mkdtemp(templ);
        ASSERT_TRUE(dir != nullptr) << "mkdtemp failed";
        m_root = std::string(dir);

        m_deletedDirs.clear();
        m_job.reset(new cm::logic::ExpiryJob(
            *m_pool, m_root,
            [this](const std::string& path) { m_deletedDirs.push_back(path); }));
        createUser();
    }

    void TearDown() override {
        m_job.reset();
        if (!m_root.empty()) {
            cm::logic::removeTree(m_root);
            m_root.clear();
        }
        cm::test::DaoTestBase::TearDown();
    }

    bool expireTask(const std::string& taskId) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "UPDATE t_transfer_task "
            "SET tr_expire_at = DATE_SUB(NOW(6), INTERVAL 1 HOUR) "
            "WHERE tr_task_id = ?");
        if (!stmt || !stmt->bindString(0, taskId)) {
            return false;
        }
        return stmt->execute();
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

    int statusOf(const std::string& taskId) {
        cm::db::TransferTaskRow row;
        cm::db::TransferDao dao(*m_conn);
        EXPECT_EQ(cm::ErrorCode::Ok, dao.findTaskByTaskId(taskId, &row));
        return static_cast<int>(row.status);
    }

    bool insertChunks(const std::string& taskId, std::size_t count) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "INSERT INTO t_chunk (c_task_id, c_index, c_offset, c_size, "
            "c_md5, c_state, c_path) VALUES (?, ?, ?, 1, ?, 1, ?)");
        if (!stmt) {
            return false;
        }
        const std::string md5 = "0123456789abcdef0123456789abcdef";
        for (std::size_t i = 0; i < count; ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "%zu.bin", i);
            const std::string path = std::string("tmp/") + taskId + "/" + name;
            if (!stmt->bindString(0, taskId) ||
                !stmt->bindUInt64(1, static_cast<std::uint64_t>(i)) ||
                !stmt->bindUInt64(2, static_cast<std::uint64_t>(i)) ||
                !stmt->bindString(3, md5) || !stmt->bindString(4, path) ||
                !stmt->execute()) {
                return false;
            }
        }
        return true;
    }

    std::uint64_t countChunks(const std::string& taskId) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "SELECT COUNT(*) FROM t_chunk WHERE c_task_id = ?");
        if (!stmt || !stmt->bindString(0, taskId) || !stmt->execute() ||
            stmt->fetchRow() != cm::db::PreparedStatement::FetchStatus::Row) {
            return 0;
        }
        return stmt->getUInt64(0);
    }

    bool dispatched(const std::string& dir) const {
        for (std::size_t i = 0; i < m_deletedDirs.size(); ++i) {
            if (m_deletedDirs[i] == dir) {
                return true;
            }
        }
        return false;
    }

    std::string m_root;
    std::vector<std::string> m_deletedDirs;
    std::unique_ptr<cm::logic::ExpiryJob> m_job;
};

TEST_F(ExpiryJobTest, ExpiryJobExpiresIdleTask) {
    const std::string taskId = createTask(m_uid, 2, 2048);
    ASSERT_FALSE(taskId.empty());
    ASSERT_TRUE(expireTask(taskId));
    ASSERT_TRUE(insertChunks(taskId, 2));

    std::size_t expired = 0;
    ASSERT_EQ(cm::ErrorCode::Ok, m_job->runOnce(&expired));
    EXPECT_GE(expired, std::size_t(1));
    EXPECT_EQ(6, statusOf(taskId));
    EXPECT_EQ(std::uint64_t(0), countChunks(taskId));
    EXPECT_TRUE(dispatched(m_root + "/tmp/" + taskId));
}

TEST_F(ExpiryJobTest, ExpiryJobKeepsMergingTask) {
    const std::string taskId = createTask(m_uid, 2, 2048);
    ASSERT_FALSE(taskId.empty());
    ASSERT_TRUE(setStatus(taskId, 7));
    ASSERT_TRUE(expireTask(taskId));
    ASSERT_TRUE(insertChunks(taskId, 1));

    std::size_t expired = 0;
    ASSERT_EQ(cm::ErrorCode::Ok, m_job->runOnce(&expired));
    EXPECT_EQ(0u, expired);
    EXPECT_EQ(7, statusOf(taskId));
    EXPECT_EQ(std::uint64_t(1), countChunks(taskId));
    EXPECT_TRUE(m_deletedDirs.empty());
}

TEST_F(ExpiryJobTest, ExpiryJobDeletesChunksInBatches) {
    const std::string taskId = createTask(m_uid, 501, 501);
    ASSERT_FALSE(taskId.empty());
    ASSERT_TRUE(expireTask(taskId));
    ASSERT_TRUE(insertChunks(taskId, 501));
    ASSERT_EQ(std::uint64_t(501), countChunks(taskId));

    std::size_t expired = 0;
    ASSERT_EQ(cm::ErrorCode::Ok, m_job->runOnce(&expired));
    EXPECT_GE(expired, std::size_t(1));
    EXPECT_EQ(6, statusOf(taskId));
    EXPECT_EQ(std::uint64_t(0), countChunks(taskId));
}

TEST_F(ExpiryJobTest, ExpiryJobRepeatsIdempotently) {
    const std::string taskId = createTask(m_uid, 1, 1);
    ASSERT_FALSE(taskId.empty());
    ASSERT_TRUE(expireTask(taskId));
    ASSERT_TRUE(insertChunks(taskId, 1));

    std::size_t first = 0;
    ASSERT_EQ(cm::ErrorCode::Ok, m_job->runOnce(&first));
    EXPECT_GE(first, std::size_t(1));
    EXPECT_EQ(6, statusOf(taskId));
    EXPECT_EQ(std::uint64_t(0), countChunks(taskId));

    std::size_t second = 0;
    ASSERT_EQ(cm::ErrorCode::Ok, m_job->runOnce(&second));
    EXPECT_EQ(0u, second);
    EXPECT_EQ(6, statusOf(taskId));
    EXPECT_TRUE(dispatched(m_root + "/tmp/" + taskId));
}

TEST_F(ExpiryJobTest, ExpiryJobRecoversStatus7OnStartup) {
    const std::string merging = createTask(m_uid, 2, 2048);
    const std::string uploading = createTask(m_uid, 2, 2048);
    ASSERT_FALSE(merging.empty());
    ASSERT_FALSE(uploading.empty());
    ASSERT_TRUE(setStatus(merging, 7));
    ASSERT_TRUE(setStatus(uploading, 1));

    std::uint64_t affected = 0;
    ASSERT_EQ(cm::ErrorCode::Ok, m_job->recoverOnStartup(&affected));
    EXPECT_GE(affected, std::uint64_t(1));
    EXPECT_EQ(2, statusOf(merging));
    EXPECT_EQ(1, statusOf(uploading));
}

TEST_F(ExpiryJobTest, ExpiryJobCleansStaleMergedTemp) {
    const std::string staleAndChunk = m_root + "/tmp/" + nextHex32();
    const std::string emptyDir = m_root + "/tmp/" + nextHex32();
    const std::string freshMerged = m_root + "/tmp/" + nextHex32();
    const std::string staleOnly = m_root + "/tmp/" + nextHex32();

    ASSERT_TRUE(ensureDir(staleAndChunk));
    ASSERT_TRUE(ensureDir(emptyDir));
    ASSERT_TRUE(ensureDir(freshMerged));
    ASSERT_TRUE(ensureDir(staleOnly));

    ASSERT_TRUE(writeFile(staleAndChunk + "/merged.bin.tmp", "stale"));
    ASSERT_TRUE(writeFile(staleAndChunk + "/0.bin", "chunk"));
    makeStale(staleAndChunk + "/merged.bin.tmp");

    ASSERT_TRUE(writeFile(freshMerged + "/merged.bin.tmp", "fresh"));

    ASSERT_TRUE(writeFile(staleOnly + "/merged.bin.tmp", "stale"));
    makeStale(staleOnly + "/merged.bin.tmp");

    std::size_t removed = 0;
    ASSERT_EQ(cm::ErrorCode::Ok, m_job->cleanStaleTmp(3600, &removed));

    EXPECT_TRUE(exists(staleAndChunk));
    EXPECT_FALSE(exists(staleAndChunk + "/merged.bin.tmp"));
    EXPECT_TRUE(exists(staleAndChunk + "/0.bin"));

    EXPECT_FALSE(exists(emptyDir));

    EXPECT_TRUE(exists(freshMerged));
    EXPECT_TRUE(exists(freshMerged + "/merged.bin.tmp"));

    EXPECT_FALSE(exists(staleOnly));
    EXPECT_EQ(std::size_t(2), removed);
}

// T-1 补漏：tick 回调只投递，不执行 SQL/IO；loop 必须始终可调度。
TEST_F(ExpiryJobTest, ExpiryJobDoesNotBlockLoop) {
    cm::ServerConfig cfg;
    cfg.dataDir = m_root;
    cfg.workerThreads = 1;

    net::EventLoop loop;
    net::TimerQueue timers(&loop);
    cm::ThreadPool workers(1, "tick-test");
    // 1ms 注入：把 tick 压到最密，任何 loop 内重活都会暴露成超时。
    cm::app::ServerApp app(cfg, *m_pool, timers, workers, 1);

    std::string error;
    ASSERT_TRUE(app.start(&error)) << error;

    std::int64_t maxUs = 0;
    for (int i = 0; i < 50; ++i) {
        const std::chrono::steady_clock::time_point begin =
            std::chrono::steady_clock::now();
        app.triggerExpiryTick();
        const std::int64_t us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - begin)
                .count();
        if (us > maxUs) {
            maxUs = us;
        }
    }
    if (maxUs > 1000) {
        std::cerr << "[WARN] expiry tick took " << maxUs
                  << "us (>1ms): tick 可能做了同步工作\n";
    }
    // 目标 <1ms；给 CI 留出 50ms 的硬上限，超出必然是把 SQL/IO 落到了 loop。
    EXPECT_LT(maxUs, 50000);

    const std::chrono::steady_clock::time_point loopStart =
        std::chrono::steady_clock::now();
    timers.runAfter(150, [&loop]() { loop.quit(); });
    loop.loop();
    const std::int64_t loopMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - loopStart)
            .count();
    EXPECT_LT(loopMs, 2000) << "loop 被 tick 阻塞";

    app.stop();
    workers.stop();
}

}  // namespace
