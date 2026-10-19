#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dao_fixture.h"
#include "db/play_dao.h"
#include "db/prepared_statement.h"

namespace {

class PlayDaoTest : public cm::test::DaoTestBase {
protected:
    void SetUp() override {
        cm::test::DaoTestBase::SetUp();
        if (m_pool == NULL) {
            return;  // 基类已 GTEST_SKIP
        }
        m_dao.reset(new cm::db::PlayDao(*m_conn));
        createUser();
    }

    std::uint64_t countRows() {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "SELECT COUNT(*) FROM t_play_record WHERE p_user_id = ?");
        if (!stmt || !stmt->bindUInt64(0, m_uid) || !stmt->execute()) {
            return 0;
        }
        if (stmt->fetchRow() != cm::db::PreparedStatement::FetchStatus::Row) {
            return 0;
        }
        return stmt->getUInt64(0);
    }

    std::uint64_t nextFileId() {
        return 800000000ULL + static_cast<std::uint64_t>(m_seq++) * 13ULL +
               static_cast<std::uint64_t>(::getpid() % 1000) * 100000ULL;
    }

    std::unique_ptr<cm::db::PlayDao> m_dao;
};

TEST_F(PlayDaoTest, PlayDaoUpsertLww) {
    const std::uint64_t fileId = nextFileId();

    cm::db::PlayRow missing;
    EXPECT_EQ(cm::ErrorCode::NotFound,
              m_dao->findByUserFile(m_uid, fileId, &missing));

    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->upsertLww(m_uid, fileId, 1000, 60000, false));

    cm::db::PlayRow row;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findByUserFile(m_uid, fileId, &row));
    ASSERT_GT(row.playId, std::uint64_t(0));
    EXPECT_EQ(m_uid, row.userId);
    EXPECT_EQ(fileId, row.fileId);
    EXPECT_EQ(std::uint64_t(1000), row.positionMs);
    EXPECT_EQ(std::uint64_t(60000), row.durationMs);
    EXPECT_FALSE(row.finished);

    // 回退进度是合法行为：LWW 直接覆盖，不做 MAX/IF 保留。
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->upsertLww(m_uid, fileId, 500, 60000, false));
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findByUserFile(m_uid, fileId, &row));
    EXPECT_EQ(std::uint64_t(500), row.positionMs);

    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->upsertLww(m_uid, fileId, 2000, 60000, true));
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findByUserFile(m_uid, fileId, &row));
    EXPECT_EQ(std::uint64_t(2000), row.positionMs);
    EXPECT_TRUE(row.finished);

    // 参数守卫。
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              m_dao->upsertLww(m_uid, fileId, 70000, 60000, false));
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              m_dao->upsertLww(0, fileId, 1, 10, false));
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              m_dao->upsertLww(m_uid, 0, 1, 10, false));
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              m_dao->findByUserFile(0, fileId, &row));
}

TEST_F(PlayDaoTest, PlayDaoOneRowPerUserFile) {
    const std::uint64_t first = nextFileId();

    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(cm::ErrorCode::Ok,
                  m_dao->upsertLww(m_uid, first, static_cast<std::uint64_t>(i) * 100,
                                   60000, false));
    }
    EXPECT_EQ(std::uint64_t(1), countRows());

    cm::db::PlayRow row;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findByUserFile(m_uid, first, &row));
    const std::uint64_t playId = row.playId;
    ASSERT_GT(playId, std::uint64_t(0));
    EXPECT_EQ(std::uint64_t(400), row.positionMs);

    // 再写一次仍复用同一行（主键不变）。
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->upsertLww(m_uid, first, 800, 60000, false));
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findByUserFile(m_uid, first, &row));
    EXPECT_EQ(playId, row.playId);
    EXPECT_EQ(std::uint64_t(1), countRows());

    std::vector<cm::db::PlayRow> recent;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->listRecent(m_uid, 10, &recent));
    ASSERT_EQ(std::size_t(1), recent.size());
    EXPECT_EQ(first, recent[0].fileId);

    // 第二个文件：多一行，且按 (updated_at, p_id) 降序稳定。
    const std::uint64_t second = nextFileId();
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->upsertLww(m_uid, second, 10, 20, false));
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->listRecent(m_uid, 10, &recent));
    ASSERT_EQ(std::size_t(2), recent.size());
    EXPECT_EQ(second, recent[0].fileId);
    EXPECT_EQ(std::uint64_t(2), countRows());

    for (std::size_t i = 1; i < recent.size(); ++i) {
        const cm::db::PlayRow& prev = recent[i - 1];
        const cm::db::PlayRow& cur = recent[i];
        EXPECT_TRUE(prev.updatedAt > cur.updatedAt ||
                    (prev.updatedAt == cur.updatedAt && prev.playId > cur.playId));
    }

    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->listRecent(m_uid, 1, &recent));
    EXPECT_EQ(std::size_t(1), recent.size());

    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->listRecent(0, 10, &recent));
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->listRecent(m_uid, 0, &recent));
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->listRecent(m_uid, 201, &recent));
}

}  // namespace
