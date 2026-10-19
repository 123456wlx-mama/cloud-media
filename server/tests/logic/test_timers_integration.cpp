#include <gtest/gtest.h>

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "EventLoop.h"
#include "TimerQueue.h"
#include "cm/base/thread_pool.h"
#include "cm/config/server_config.h"
#include "dao_fixture.h"
#include "db/file_dao.h"
#include "db/prepared_statement.h"
#include "logic/jobs/orphan_gc_job.h"
#include "logic/upload_fs.h"
#include "app/server_app.h"

namespace {

class TimersIntegrationTest : public cm::test::DaoTestBase {
protected:
    void SetUp() override {
        cm::test::DaoTestBase::SetUp();
        if (m_pool == NULL) {
            return;  // 基类已 GTEST_SKIP（缺 MySQL）
        }
        char templ[] = "/tmp/cm_timers_XXXXXX";
        char* dir = ::mkdtemp(templ);
        ASSERT_TRUE(dir != NULL) << "mkdtemp failed";
        m_root = std::string(dir);
        createUser();
    }

    void TearDown() override {
        if (!m_root.empty()) {
            cm::logic::removeTree(m_root);
            m_root.clear();
        }
        cm::test::DaoTestBase::TearDown();
    }

    void expireAllTasks() {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "UPDATE t_transfer_task "
            "SET tr_expire_at = DATE_SUB(NOW(6), INTERVAL 1 HOUR) "
            "WHERE tr_user_id = ? AND tr_status IN (0, 1, 2)");
        ASSERT_TRUE(stmt);
        ASSERT_TRUE(stmt->bindUInt64(0, m_uid));
        ASSERT_TRUE(stmt->execute());
    }

    std::uint64_t countStatus(std::uint8_t status) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "SELECT COUNT(*) FROM t_transfer_task "
            "WHERE tr_user_id = ? AND tr_status = ?");
        if (!stmt || !stmt->bindUInt64(0, m_uid) ||
            !stmt->bindUInt64(1, status) || !stmt->execute()) {
            return 0;
        }
        if (stmt->fetchRow() != cm::db::PreparedStatement::FetchStatus::Row) {
            return 0;
        }
        return stmt->getUInt64(0);
    }

    void makeOrphan(std::uint64_t fid, std::uint64_t secondsAgo) {
        // 宽限基准是 f_updated_at（孤儿时刻），必须显式回拨；
        // 显式赋值优先于 ON UPDATE，否则会被刷成 NOW。
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "UPDATE t_file SET f_state = 2, f_ref_count = 0, "
            "f_created_at = DATE_SUB(NOW(6), INTERVAL ? SECOND), "
            "f_updated_at = DATE_SUB(NOW(6), INTERVAL ? SECOND) "
            "WHERE f_id = ?");
        ASSERT_TRUE(stmt);
        ASSERT_TRUE(stmt->bindUInt64(0, secondsAgo));
        ASSERT_TRUE(stmt->bindUInt64(1, secondsAgo));
        ASSERT_TRUE(stmt->bindUInt64(2, fid));
        ASSERT_TRUE(stmt->execute());
    }

    bool blobExists(std::uint64_t fid) {
        cm::db::BlobRow row;
        return cm::db::FileDao(*m_conn).getBlobById(fid, &row) ==
               cm::ErrorCode::Ok;
    }

    cm::ServerConfig cfg() const {
        cm::ServerConfig c;
        c.dataDir = m_root;
        c.workerThreads = 2;
        return c;
    }

    std::string m_root;
};

// 定时器两轮：250 个过期任务 → 第一轮 200、第二轮 50，最终每个任务只处理一次。
TEST_F(TimersIntegrationTest, TimerExpiryTwoRounds) {
    for (int i = 0; i < 250; ++i) {
        ASSERT_FALSE(createTask(m_uid, 1, 64).empty());
    }
    expireAllTasks();

    net::EventLoop loop;
    net::TimerQueue timers(&loop);
    cm::ThreadPool workers(2, "timers-test");
    cm::ServerConfig config = cfg();
    cm::app::ServerApp app(config, *m_pool, timers, workers, 1000);

    std::string error;
    ASSERT_TRUE(app.start(&error)) << error;

    std::uint64_t processed = 0;
    bool finished = false;
    timers.runEvery(100, [&]() {
        processed = countStatus(6);
        if (processed == 250) {
            finished = true;
            loop.quit();
        }
    });
    timers.runAfter(25000, [&]() { loop.quit(); });  // 看门狗

    const std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
    loop.loop();
    const std::int64_t elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();

    app.stop();
    workers.stop();

    EXPECT_TRUE(finished) << "expiry did not finish in 25s, status6="
                          << countStatus(6);
    EXPECT_EQ(std::uint64_t(250), countStatus(6));
    EXPECT_EQ(std::uint64_t(0), countStatus(0) + countStatus(1) + countStatus(2));
    // 两轮 tick（1s 间隔），不会在启动时同步跑完。
    EXPECT_GE(elapsedMs, 900);
    EXPECT_LT(elapsedMs, 20000);
}

TEST_F(TimersIntegrationTest, TimerGcBatch) {
    std::vector<std::uint64_t> fids;
    for (int i = 0; i < 150; ++i) {
        const std::uint64_t fid = createBlob(16);
        ASSERT_GT(fid, std::uint64_t(0));
        fids.push_back(fid);
        makeOrphan(fid, 25 * 3600);
    }

    cm::logic::OrphanGcJob job(*m_pool, m_root,
                               [](const std::string&) { return true; });
    std::size_t deleted = 0;
    ASSERT_EQ(cm::ErrorCode::Ok, job.runOnce(&deleted, NULL));
    EXPECT_EQ(std::size_t(100), deleted);

    std::size_t remaining = 0;
    for (std::size_t i = 0; i < fids.size(); ++i) {
        if (blobExists(fids[i])) {
            ++remaining;
        }
    }
    EXPECT_EQ(std::size_t(50), remaining);
}

// 停机路径：tick 运行中 stop，必须在 5s 内退出且不崩溃（ASan 下无 use-after-free）。
TEST_F(TimersIntegrationTest, TimerShutdownNoLeak) {
    net::EventLoop loop;
    net::TimerQueue timers(&loop);
    cm::ThreadPool workers(2, "timers-stop");
    cm::ServerConfig config = cfg();
    cm::app::ServerApp app(config, *m_pool, timers, workers, 100);

    std::string error;
    ASSERT_TRUE(app.start(&error)) << error;

    timers.runAfter(500, [&loop]() { loop.quit(); });
    loop.loop();

    const std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
    app.stop();
    workers.stop();
    const std::int64_t stopMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();
    EXPECT_LT(stopMs, 5000);
}

}  // namespace
