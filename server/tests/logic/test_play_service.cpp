#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

#include "dao_fixture.h"
#include "db/file_dao.h"
#include "db/play_dao.h"
#include "db/prepared_statement.h"
#include "logic/play_service.h"

namespace {

class PlayServiceTest : public cm::test::DaoTestBase {
protected:
    void SetUp() override {
        cm::test::DaoTestBase::SetUp();
        if (m_pool == NULL) {
            return;  // 基类已 GTEST_SKIP（缺 MySQL）
        }
        m_service.reset(new cm::logic::PlayService(*m_pool));
        createUser();
    }

    void TearDown() override {
        m_service.reset();
        cm::test::DaoTestBase::TearDown();
    }

    std::uint64_t createNode(const std::string& name, std::uint64_t fid) {
        return DaoTestBase::createNode(m_uid, name, fid, NULL);
    }

    cm::protocol::PlayReportRq makeRq(std::uint64_t fid, std::uint64_t position,
                                      std::uint64_t duration,
                                      std::uint8_t finished) {
        cm::protocol::PlayReportRq rq;
        rq.fId = fid;
        rq.positionMs = position;
        rq.durationMs = duration;
        rq.finished = finished;
        return rq;
    }

    std::uint64_t countRows(std::uint64_t uid) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "SELECT COUNT(*) FROM t_play_record WHERE p_user_id = ?");
        if (!stmt || !stmt->bindUInt64(0, uid) || !stmt->execute()) {
            return 0;
        }
        if (stmt->fetchRow() != cm::db::PreparedStatement::FetchStatus::Row) {
            return 0;
        }
        return stmt->getUInt64(0);
    }

    cm::db::PlayRow findRow(std::uint64_t uid, std::uint64_t fid) {
        cm::db::PlayRow row;
        cm::db::PlayDao(*m_conn).findByUserFile(uid, fid, &row);
        return row;
    }

    std::unique_ptr<cm::logic::PlayService> m_service;
};

TEST_F(PlayServiceTest, PlayCreate) {
    const std::uint64_t fid = createBlob(8192);
    ASSERT_GT(createNode("movie.mp4", fid), std::uint64_t(0));

    cm::protocol::PlayReportRq rq = makeRq(fid, 1000, 60000, 0);
    cm::protocol::PlayReportRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok, m_service->report(m_uid, rq, &rs));
    EXPECT_EQ(std::uint32_t(0), rs.code);
    EXPECT_EQ(std::uint8_t(1), rs.saved);

    const cm::db::PlayRow row = findRow(m_uid, fid);
    EXPECT_EQ(m_uid, row.userId);
    EXPECT_EQ(fid, row.fileId);
    EXPECT_EQ(std::uint64_t(1000), row.positionMs);
    EXPECT_EQ(std::uint64_t(60000), row.durationMs);
    EXPECT_FALSE(row.finished);
    EXPECT_EQ(std::uint64_t(1), countRows(m_uid));
}

TEST_F(PlayServiceTest, PlayUpdateLww) {
    const std::uint64_t fid = createBlob(8192);
    ASSERT_GT(createNode("movie.mp4", fid), std::uint64_t(0));

    cm::protocol::PlayReportRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->report(m_uid, makeRq(fid, 1000, 60000, 0), &rs));
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->report(m_uid, makeRq(fid, 2000, 60000, 0), &rs));
    EXPECT_EQ(std::uint8_t(1), rs.saved);

    const cm::db::PlayRow row = findRow(m_uid, fid);
    EXPECT_EQ(std::uint64_t(2000), row.positionMs);
    EXPECT_EQ(std::uint64_t(1), countRows(m_uid));
}

TEST_F(PlayServiceTest, PlayRewindAllowed) {
    const std::uint64_t fid = createBlob(8192);
    ASSERT_GT(createNode("movie.mp4", fid), std::uint64_t(0));

    cm::protocol::PlayReportRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->report(m_uid, makeRq(fid, 4000, 60000, 0), &rs));
    // 回退进度是合法行为：LWW 直接覆盖，不用 MAX/IF。
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->report(m_uid, makeRq(fid, 500, 60000, 0), &rs));
    EXPECT_EQ(std::uint8_t(1), rs.saved);

    const cm::db::PlayRow row = findRow(m_uid, fid);
    EXPECT_EQ(std::uint64_t(500), row.positionMs);
}

TEST_F(PlayServiceTest, PlayInvalidPosition) {
    const std::uint64_t fid = createBlob(8192);
    ASSERT_GT(createNode("movie.mp4", fid), std::uint64_t(0));

    cm::protocol::PlayReportRs rs;
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              m_service->report(m_uid, makeRq(fid, 70000, 60000, 0), &rs));
    EXPECT_EQ(std::uint32_t(1001), rs.code);
    EXPECT_EQ(std::uint8_t(0), rs.saved);
    EXPECT_EQ(std::uint64_t(0), countRows(m_uid));

    // f_id 不存在 → 1002。
    EXPECT_EQ(cm::ErrorCode::NotFound,
              m_service->report(m_uid, makeRq(999999999999ULL, 1, 10, 0), &rs));
    EXPECT_EQ(std::uint32_t(1002), rs.code);

    // 别人的文件同样 1002：不泄露存在性。
    const std::uint64_t otherFid = createBlob(1024);
    ASSERT_GT(createNode("other.bin", otherFid), std::uint64_t(0));
    const std::uint64_t owner = m_uid;
    createUser();  // 切换成第二个用户
    EXPECT_EQ(cm::ErrorCode::NotFound,
              m_service->report(m_uid, makeRq(otherFid, 1, 10, 0), &rs));
    EXPECT_EQ(std::uint32_t(1002), rs.code);
    EXPECT_EQ(std::uint64_t(0), countRows(m_uid));
    EXPECT_EQ(std::uint64_t(0), countRows(owner));
}

TEST_F(PlayServiceTest, PlayOneRow) {
    const std::uint64_t fid = createBlob(8192);
    ASSERT_GT(createNode("movie.mp4", fid), std::uint64_t(0));

    cm::protocol::PlayReportRs rs;
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(cm::ErrorCode::Ok,
                  m_service->report(m_uid, makeRq(fid, 3000, 60000, 0), &rs));
    }
    EXPECT_EQ(std::uint64_t(1), countRows(m_uid));

    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->report(m_uid, makeRq(fid, 3000, 60000, 1), &rs));
    EXPECT_EQ(std::uint64_t(1), countRows(m_uid));
    const cm::db::PlayRow row = findRow(m_uid, fid);
    EXPECT_TRUE(row.finished);
    EXPECT_EQ(std::uint64_t(3000), row.positionMs);
}

}  // namespace
