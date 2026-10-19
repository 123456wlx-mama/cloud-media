#include <gtest/gtest.h>

#include <cctype>
#include <cstdint>
#include <memory>
#include <string>

#include "dao_fixture.h"
#include "db/play_dao.h"
#include "db/prepared_statement.h"
#include "db/share_dao.h"

namespace {

class DaoFixtureTest : public cm::test::DaoTestBase {
protected:
    std::uint64_t countByUInt(const char* sql, std::uint64_t value) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(sql);
        if (!stmt || !stmt->bindUInt64(0, value) || !stmt->execute()) {
            return 0;
        }
        if (stmt->fetchRow() != cm::db::PreparedStatement::FetchStatus::Row) {
            return 0;
        }
        return stmt->getUInt64(0);
    }

    // 前缀覆盖：UserDao 用例直接用 DAO 插用户，只有 tel 前缀能兜住。
    std::uint64_t countUsersByPrefix() {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "SELECT COUNT(*) FROM t_user WHERE u_tel LIKE ?");
        if (!stmt || !stmt->bindString(0, prefix() + "%") || !stmt->execute()) {
            return 0;
        }
        if (stmt->fetchRow() != cm::db::PreparedStatement::FetchStatus::Row) {
            return 0;
        }
        return stmt->getUInt64(0);
    }
};

TEST_F(DaoFixtureTest, DaoFixtureSmoke) {
    ASSERT_GT(createUser(), std::uint64_t(0));
    const std::uint64_t fid = createBlob(1024);
    ASSERT_GT(fid, std::uint64_t(0));
    const std::uint64_t ufId = createNode(m_uid, "smoke.bin", fid);
    ASSERT_GT(ufId, std::uint64_t(0));
    const std::string taskId = createTask(m_uid, 2, 8192);
    ASSERT_FALSE(taskId.empty());
    const std::string code = createShare(m_uid, ufId, false, fid, "", 2);
    ASSERT_FALSE(code.empty());

    // 播放记录也纳入清理范围。
    cm::db::PlayDao play(*m_conn);
    ASSERT_EQ(cm::ErrorCode::Ok, play.upsertLww(m_uid, fid, 10, 20, false));

    EXPECT_EQ(std::uint64_t(1), countUsersByPrefix());
    EXPECT_EQ(std::uint64_t(1),
              countByUInt("SELECT COUNT(*) FROM t_user_file WHERE u_id = ?", m_uid));
    EXPECT_EQ(std::uint64_t(1),
              countByUInt("SELECT COUNT(*) FROM t_transfer_task WHERE tr_user_id = ?",
                          m_uid));
    EXPECT_EQ(std::uint64_t(1),
              countByUInt("SELECT COUNT(*) FROM t_share WHERE s_user_id = ?", m_uid));
    EXPECT_EQ(std::uint64_t(1),
              countByUInt("SELECT COUNT(*) FROM t_play_record WHERE p_user_id = ?",
                          m_uid));
    EXPECT_EQ(std::uint64_t(1),
              countByUInt("SELECT COUNT(*) FROM t_file WHERE f_id = ?", fid));

    cleanup();

    EXPECT_EQ(std::uint64_t(0), countUsersByPrefix());
    EXPECT_EQ(std::uint64_t(0),
              countByUInt("SELECT COUNT(*) FROM t_user_file WHERE u_id = ?", m_uid));
    EXPECT_EQ(std::uint64_t(0),
              countByUInt("SELECT COUNT(*) FROM t_transfer_task WHERE tr_user_id = ?",
                          m_uid));
    EXPECT_EQ(std::uint64_t(0),
              countByUInt("SELECT COUNT(*) FROM t_share WHERE s_user_id = ?", m_uid));
    EXPECT_EQ(std::uint64_t(0),
              countByUInt("SELECT COUNT(*) FROM t_play_record WHERE p_user_id = ?",
                          m_uid));
    EXPECT_EQ(std::uint64_t(0),
              countByUInt("SELECT COUNT(*) FROM t_file WHERE f_id = ?", fid));
}

TEST_F(DaoFixtureTest, DaoFixtureUniquePrefix) {
    EXPECT_FALSE(prefix().empty());
    EXPECT_EQ(std::uint64_t(0), m_uid);

    const std::string first = nextTel();
    const std::string second = nextTel();
    EXPECT_NE(first, second);
    EXPECT_EQ(0, first.compare(0, prefix().size(), prefix()));
    EXPECT_EQ(0, second.compare(0, prefix().size(), prefix()));
    EXPECT_LE(first.size(), std::size_t(20));  // u_tel VARCHAR(20)

    const std::string hexA = nextHex32();
    const std::string hexB = nextHex32();
    EXPECT_EQ(std::size_t(32), hexA.size());
    EXPECT_NE(hexA, hexB);
    for (std::size_t i = 0; i < hexA.size(); ++i) {
        EXPECT_NE(0, std::isxdigit(static_cast<unsigned char>(hexA[i])));
    }

    const std::string code = nextCode();
    EXPECT_TRUE(cm::db::ShareDao::isValidCode(code));
    EXPECT_EQ(std::size_t(6), code.size());
}

}  // namespace
