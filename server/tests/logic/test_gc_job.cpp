#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "dao_fixture.h"
#include "db/file_dao.h"
#include "db/play_dao.h"
#include "db/prepared_statement.h"
#include "logic/jobs/orphan_gc_job.h"
#include "logic/jobs/play_cleanup_job.h"
#include "logic/jobs/share_expiry_job.h"
#include "logic/upload_fs.h"

namespace {

bool ensureDir(const std::string& path) {
    std::string current;
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

class GcJobTest : public cm::test::DaoTestBase {
protected:
    void SetUp() override {
        cm::test::DaoTestBase::SetUp();
        if (m_pool == nullptr) {
            return;  // 基类已 GTEST_SKIP（缺 MySQL）
        }

        char templ[] = "/tmp/cm_gc_XXXXXX";
        char* dir = ::mkdtemp(templ);
        ASSERT_TRUE(dir != nullptr) << "mkdtemp failed";
        m_root = std::string(dir);

        m_failUnlink = false;
        m_job.reset(new cm::logic::OrphanGcJob(
            *m_pool, m_root,
            [this](const std::string& path) {
                if (m_failUnlink) {
                    return false;
                }
                m_unlinkCalls.push_back(path);
                if (::unlink(path.c_str()) == 0) {
                    return true;
                }
                return errno == ENOENT;
            }));
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

    // 必须是孤儿时刻（f_updated_at）回拨；显式赋值优先于 ON UPDATE，
    // 否则本条 UPDATE 会把 f_updated_at 刷成 NOW，宽限测试就失去意义。
    void makeOrphanWithTimes(std::uint64_t fid, std::uint64_t createdAgo,
                             std::uint64_t updatedAgo) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "UPDATE t_file SET f_state = 2, f_ref_count = 0, "
            "f_created_at = DATE_SUB(NOW(6), INTERVAL ? SECOND), "
            "f_updated_at = DATE_SUB(NOW(6), INTERVAL ? SECOND) "
            "WHERE f_id = ?");
        ASSERT_TRUE(stmt) << m_conn->lastErrorMessage();
        ASSERT_TRUE(stmt->bindUInt64(0, createdAgo));
        ASSERT_TRUE(stmt->bindUInt64(1, updatedAgo));
        ASSERT_TRUE(stmt->bindUInt64(2, fid));
        ASSERT_TRUE(stmt->execute()) << stmt->lastErrorMessage();
    }

    void makeOrphan(std::uint64_t fid, std::uint64_t secondsAgo) {
        makeOrphanWithTimes(fid, secondsAgo, secondsAgo);
    }

    // 按 f_path 落出真实 blob 与 .crc sidecar，返回绝对路径。
    std::string makeBlobFiles(std::uint64_t fid) {
        cm::db::BlobRow row;
        EXPECT_EQ(cm::ErrorCode::Ok,
                  cm::db::FileDao(*m_conn).getBlobById(fid, &row));
        const std::string path = m_root + "/blobs/" + row.path;
        const std::string dir = path.substr(0, path.find_last_of('/'));
        EXPECT_TRUE(ensureDir(dir));
        EXPECT_TRUE(writeFile(path, std::string(static_cast<std::size_t>(row.size), 'x')));
        EXPECT_TRUE(writeFile(path + ".crc", "crc"));
        return path;
    }

    bool blobExists(std::uint64_t fid) {
        cm::db::BlobRow row;
        return cm::db::FileDao(*m_conn).getBlobById(fid, &row) ==
               cm::ErrorCode::Ok;
    }

    void softDeleteNodeDirect(std::uint64_t ufId) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "UPDATE t_user_file SET uf_deleted_at = NOW(6) WHERE uf_id = ?");
        ASSERT_TRUE(stmt);
        ASSERT_TRUE(stmt->bindUInt64(0, ufId));
        ASSERT_TRUE(stmt->execute());
    }

    void expireShare(const std::string& code) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "UPDATE t_share SET s_expire_at = DATE_SUB(NOW(6), INTERVAL 1 HOUR) "
            "WHERE s_code = ?");
        ASSERT_TRUE(stmt);
        ASSERT_TRUE(stmt->bindString(0, code));
        ASSERT_TRUE(stmt->execute());
    }

    int shareStatus(const std::string& code) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "SELECT s_status FROM t_share WHERE s_code = ?");
        if (!stmt || !stmt->bindString(0, code) || !stmt->execute()) {
            return -1;
        }
        if (stmt->fetchRow() != cm::db::PreparedStatement::FetchStatus::Row) {
            return -1;
        }
        return static_cast<int>(stmt->getUInt64(0));
    }

    std::string m_root;
    bool m_failUnlink;
    std::vector<std::string> m_unlinkCalls;
    std::unique_ptr<cm::logic::OrphanGcJob> m_job;
};

TEST_F(GcJobTest, GcSkipsWithinGrace) {
    const std::uint64_t fid = createBlob(1024);
    ASSERT_GT(fid, std::uint64_t(0));
    makeOrphan(fid, 3600);  // 1h，不足 24h 宽限

    std::size_t deleted = 99;
    ASSERT_EQ(cm::ErrorCode::Ok, m_job->runOnce(&deleted, nullptr));
    EXPECT_EQ(std::size_t(0), deleted);
    EXPECT_TRUE(blobExists(fid));
}

TEST_F(GcJobTest, GcSkipsRecentlyOrphanedOldBlob) {
    const std::uint64_t fid = createBlob(2048);
    ASSERT_GT(fid, std::uint64_t(0));
    // 30 天前创建、1 小时前才变孤儿：宽限必须保护它。
    makeOrphanWithTimes(fid, 30 * 24 * 3600, 3600);

    std::size_t deleted = 99;
    ASSERT_EQ(cm::ErrorCode::Ok, m_job->runOnce(&deleted, nullptr));
    EXPECT_EQ(std::size_t(0), deleted);
    EXPECT_TRUE(blobExists(fid));
}

TEST_F(GcJobTest, GcSkipsActiveNode) {
    const std::uint64_t fid = createBlob(1024);
    ASSERT_GT(createNode(m_uid, "alive.bin", fid, nullptr), std::uint64_t(0));
    makeOrphan(fid, 25 * 3600);

    std::size_t deleted = 99;
    ASSERT_EQ(cm::ErrorCode::Ok, m_job->runOnce(&deleted, nullptr));
    EXPECT_EQ(std::size_t(0), deleted);
    EXPECT_TRUE(blobExists(fid));
}

TEST_F(GcJobTest, GcSkipsActiveShare) {
    const std::uint64_t fid = createBlob(1024);
    const std::uint64_t ufId = createNode(m_uid, "shared.bin", fid, nullptr);
    ASSERT_GT(ufId, std::uint64_t(0));
    const std::string code = createShare(m_uid, ufId, false, fid, "", 0);
    ASSERT_EQ(std::size_t(6), code.size());
    // 去掉活跃节点，只留有效分享挡住 GC。
    softDeleteNodeDirect(ufId);
    makeOrphan(fid, 25 * 3600);

    std::size_t deleted = 99;
    ASSERT_EQ(cm::ErrorCode::Ok, m_job->runOnce(&deleted, nullptr));
    EXPECT_EQ(std::size_t(0), deleted);
    EXPECT_TRUE(blobExists(fid));
    EXPECT_EQ(0, shareStatus(code));
}

TEST_F(GcJobTest, GcDeletesBlobAndSidecar) {
    const std::uint64_t fid = createBlob(4096);
    const std::string path = makeBlobFiles(fid);
    ASSERT_TRUE(exists(path));
    ASSERT_TRUE(exists(path + ".crc"));
    makeOrphan(fid, 25 * 3600);

    std::size_t deleted = 0;
    std::uint64_t bytes = 0;
    ASSERT_EQ(cm::ErrorCode::Ok, m_job->runOnce(&deleted, &bytes));
    EXPECT_EQ(std::size_t(1), deleted);
    EXPECT_EQ(std::uint64_t(4096), bytes);
    EXPECT_FALSE(exists(path));
    EXPECT_FALSE(exists(path + ".crc"));
    EXPECT_FALSE(blobExists(fid));
    ASSERT_EQ(std::size_t(2), m_unlinkCalls.size());
}

TEST_F(GcJobTest, GcUnlinkFailureKeepsRow) {
    const std::uint64_t fid = createBlob(512);
    const std::string path = makeBlobFiles(fid);
    makeOrphan(fid, 25 * 3600);
    m_failUnlink = true;

    std::size_t deleted = 99;
    ASSERT_EQ(cm::ErrorCode::Ok, m_job->runOnce(&deleted, nullptr));
    EXPECT_EQ(std::size_t(0), deleted);
    EXPECT_TRUE(exists(path));
    EXPECT_TRUE(blobExists(fid));
}

TEST_F(GcJobTest, GcBatchLimit100) {
    std::vector<std::uint64_t> fids;
    for (int i = 0; i < 120; ++i) {
        const std::uint64_t fid = createBlob(10);
        ASSERT_GT(fid, std::uint64_t(0));
        fids.push_back(fid);
        makeOrphan(fid, 25 * 3600);
    }

    std::size_t deleted = 0;
    ASSERT_EQ(cm::ErrorCode::Ok, m_job->runOnce(&deleted, nullptr));
    EXPECT_EQ(std::size_t(100), deleted);

    std::size_t remaining = 0;
    for (std::size_t i = 0; i < fids.size(); ++i) {
        if (blobExists(fids[i])) {
            ++remaining;
        }
    }
    EXPECT_EQ(std::size_t(20), remaining);
}

TEST_F(GcJobTest, ShareExpirySweep) {
    const std::uint64_t fid = createBlob(64);
    const std::uint64_t ufId = createNode(m_uid, "s.bin", fid, nullptr);
    ASSERT_GT(ufId, std::uint64_t(0));
    const std::string expired = createShare(m_uid, ufId, false, fid, "", 0);
    const std::string active = createShare(m_uid, ufId, false, fid, "", 2);
    ASSERT_EQ(std::size_t(6), expired.size());
    ASSERT_EQ(std::size_t(6), active.size());
    expireShare(expired);

    cm::logic::ShareExpiryJob job(*m_pool);
    std::uint64_t affected = 0;
    ASSERT_EQ(cm::ErrorCode::Ok, job.runOnce(&affected));
    EXPECT_GE(affected, std::uint64_t(1));
    EXPECT_EQ(1, shareStatus(expired));
    EXPECT_EQ(0, shareStatus(active));
}

TEST_F(GcJobTest, PlayCleanupOrphans) {
    const std::uint64_t missingFid = 900000000000ULL +
                                     static_cast<std::uint64_t>(::getpid()) *
                                         1000ULL;
    const std::uint64_t realFid = createBlob(128);
    ASSERT_GT(realFid, std::uint64_t(0));

    cm::db::PlayDao dao(*m_conn);
    ASSERT_EQ(cm::ErrorCode::Ok,
              dao.upsertLww(m_uid, missingFid, 1, 10, false));
    ASSERT_EQ(cm::ErrorCode::Ok,
              dao.upsertLww(m_uid, realFid, 2, 10, false));

    cm::logic::PlayCleanupJob job(*m_pool, 500);
    std::uint64_t deleted = 0;
    ASSERT_EQ(cm::ErrorCode::Ok, job.runOnce(&deleted));
    EXPECT_GE(deleted, std::uint64_t(1));

    cm::db::PlayRow row;
    EXPECT_EQ(cm::ErrorCode::NotFound,
              dao.findByUserFile(m_uid, missingFid, &row));
    EXPECT_EQ(cm::ErrorCode::Ok, dao.findByUserFile(m_uid, realFid, &row));
}

}  // namespace
