#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "dao_fixture.h"
#include "db/share_dao.h"

namespace {

class ShareDaoTest : public cm::test::DaoTestBase {
protected:
    void SetUp() override {
        cm::test::DaoTestBase::SetUp();
        if (m_pool == nullptr) {
            return;  // 基类已 GTEST_SKIP
        }
        m_dao.reset(new cm::db::ShareDao(*m_conn));
        createUser();
        m_ufId = nextUfId();
    }

    // uf_id / f_id 只需唯一：t_share 没有外键。
    // 用前缀派生，保证不同用例实例不会共用同一个 uf_id（listActiveByUfId 按它过滤）。
    std::uint64_t nextUfId() {
        std::uint64_t base = 0;
        const std::string& p = prefix();
        for (std::size_t i = 0; i < p.size(); ++i) {
            base = base * 131ULL + static_cast<unsigned char>(p[i]);
        }
        return 900000000ULL + (base % 1000000ULL) * 1000ULL +
               static_cast<std::uint64_t>(m_seq++);
    }

    // 指定分享码插入（重复码用例需要固定 code）。
    std::string insertWithCode(const std::string& code,
                               std::uint64_t ufId,
                               bool isDir,
                               std::uint64_t fileId,
                               const std::string& accessCode,
                               std::uint8_t policy) {
        cm::db::NewShare share;
        share.code = code;
        share.userId = m_uid;
        share.ufId = ufId;
        share.fileId = fileId;
        share.isDir = isDir;
        share.accessCode = accessCode;
        share.expirePolicy = policy;

        std::uint64_t shareId = 0;
        EXPECT_EQ(cm::ErrorCode::Ok, m_dao->insert(share, &shareId));
        trackShare(code);
        return code;
    }

    std::string createFileShare(std::uint64_t ufId) {
        return createShare(m_uid, ufId, false, nextUfId(), "", 2);
    }

    std::vector<cm::db::ShareRow> collectAllPages(cm::db::ShareListQuery query,
                                                  std::size_t* pages) {
        std::vector<cm::db::ShareRow> all;
        *pages = 0;
        for (;;) {
            cm::db::ShareListPage page;
            const cm::ErrorCode rc = m_dao->listByUser(query, &page);
            EXPECT_EQ(cm::ErrorCode::Ok, rc);
            if (rc != cm::ErrorCode::Ok) {
                break;
            }
            ++(*pages);
            for (std::size_t i = 0; i < page.rows.size(); ++i) {
                all.push_back(page.rows[i]);
            }
            if (!page.hasMore) {
                break;
            }
            query.cursorCreatedAt = page.nextCreatedAt;
            query.cursorId = page.nextId;
            if (all.size() > 1000) {
                ADD_FAILURE() << "pagination did not terminate";
                break;
            }
        }
        return all;
    }

    std::unique_ptr<cm::db::ShareDao> m_dao;
    std::uint64_t m_ufId = 0;
};

TEST_F(ShareDaoTest, ShareDaoInsertDuplicateCode) {
    const std::string code = nextCode();
    const std::uint64_t fileId = nextUfId();
    std::uint64_t shareId = 0;

    cm::db::NewShare share;
    share.code = code;
    share.userId = m_uid;
    share.ufId = m_ufId;
    share.fileId = fileId;
    share.isDir = false;
    share.accessCode = "1234";
    share.expirePolicy = 2;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->insert(share, &shareId));
    ASSERT_GT(shareId, std::uint64_t(0));

    // 同码再插：DAO 不换码、不重试，原样把 1062 交给服务层。
    cm::db::NewShare duplicate = share;
    duplicate.ufId = nextUfId();
    std::uint64_t duplicateId = 123;
    EXPECT_EQ(cm::ErrorCode::AlreadyExists, m_dao->insert(duplicate, &duplicateId));
    EXPECT_EQ(std::uint64_t(0), duplicateId);

    // 目录分享：s_file_id 为 NULL，读回 0。
    cm::db::NewShare dirShare;
    dirShare.code = nextCode();
    dirShare.userId = m_uid;
    dirShare.ufId = nextUfId();
    dirShare.isDir = true;
    dirShare.expirePolicy = 2;
    std::uint64_t dirId = 0;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->insert(dirShare, &dirId));
    cm::db::ShareRow dirRow;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findByCode(dirShare.code, &dirRow));
    EXPECT_EQ(std::uint64_t(0), dirRow.fileId);
    EXPECT_TRUE(dirRow.accessCode.empty());

    // 目录带 file_id、文件不带 file_id：都拒绝。
    cm::db::NewShare badDir = dirShare;
    badDir.code = nextCode();
    badDir.fileId = fileId;
    std::uint64_t ignored = 0;
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->insert(badDir, &ignored));

    cm::db::NewShare badFile = share;
    badFile.code = nextCode();
    badFile.fileId = 0;
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->insert(badFile, &ignored));

    // 非法分享码 / 非法提取码。
    cm::db::NewShare badCode = share;
    badCode.code = "ABCDE0";  // 0 不在字符表
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->insert(badCode, &ignored));
    badCode.code = "ABC23";  // 长度 5
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->insert(badCode, &ignored));
    badCode.code = "abc234";  // 小写
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->insert(badCode, &ignored));

    cm::db::NewShare badAccess = share;
    badAccess.code = nextCode();
    badAccess.accessCode = "12a4";
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->insert(badAccess, &ignored));
}

TEST_F(ShareDaoTest, ShareDaoFindByCode) {
    const std::string code = nextCode();
    const std::uint64_t fileId = nextUfId();
    insertWithCode(code, m_ufId, false, fileId, "5678", 2);

    cm::db::ShareRow missing;
    EXPECT_EQ(cm::ErrorCode::NotFound, m_dao->findByCode(nextCode(), &missing));

    cm::db::ShareRow row;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findByCode(code, &row));
    EXPECT_EQ(code, row.code);
    EXPECT_EQ(m_uid, row.userId);
    EXPECT_EQ(m_ufId, row.ufId);
    EXPECT_EQ(fileId, row.fileId);
    EXPECT_EQ(std::string("5678"), row.accessCode);
    EXPECT_EQ(0, static_cast<int>(row.status));
    EXPECT_FALSE(row.expireAt.empty());
    EXPECT_FALSE(row.createdAt.empty());
    // 7 天档：过期时间晚于创建时间（DATETIME(6) 文本可按字典序比较）。
    EXPECT_LT(row.createdAt, row.expireAt);

    // 永久分享写 9999 哨兵。
    const std::string foreverCode = nextCode();
    insertWithCode(foreverCode, nextUfId(), false, nextUfId(), "", 0);
    cm::db::ShareRow forever;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findByCode(foreverCode, &forever));
    EXPECT_NE(std::string::npos, forever.expireAt.find("9999"));

    // 非法码格式直接拒绝，不打到 DB。
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->findByCode("abc234", &row));
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->findByCode("", &row));
    EXPECT_TRUE(cm::db::ShareDao::isValidCode(code));
    EXPECT_FALSE(cm::db::ShareDao::isValidCode("ABCDE0"));
}

TEST_F(ShareDaoTest, ShareDaoListCursor) {
    for (int i = 0; i < 7; ++i) {
        createFileShare(m_ufId);
    }

    cm::db::ShareListQuery query;
    query.userId = m_uid;
    query.limit = 3;

    std::size_t pages = 0;
    const std::vector<cm::db::ShareRow> rows = collectAllPages(query, &pages);
    ASSERT_EQ(std::size_t(7), rows.size());
    EXPECT_EQ(std::size_t(3), pages);

    std::set<std::uint64_t> seen;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        EXPECT_TRUE(seen.insert(rows[i].shareId).second) << "duplicate share id";
        if (i > 0) {
            const cm::db::ShareRow& prev = rows[i - 1];
            const cm::db::ShareRow& cur = rows[i];
            // (created_at, s_id) 严格递减；同微秒时由 s_id 兜底。
            EXPECT_TRUE(prev.createdAt > cur.createdAt ||
                        (prev.createdAt == cur.createdAt &&
                         prev.shareId > cur.shareId));
        }
    }

    // 取消一条后不再出现在“我的分享”。
    const std::string cancelledCode = rows.front().code;
    bool cancelled = false;
    bool already = false;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->cancel(m_uid, cancelledCode, &cancelled, &already));
    ASSERT_TRUE(cancelled);

    std::size_t pagesAfter = 0;
    cm::db::ShareListQuery queryAfter = query;
    const std::vector<cm::db::ShareRow> after = collectAllPages(queryAfter, &pagesAfter);
    EXPECT_EQ(std::size_t(6), after.size());

    // 参数守卫。
    cm::db::ShareListPage page;
    cm::db::ShareListQuery bad = query;
    bad.limit = 0;
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->listByUser(bad, &page));
    bad.limit = 201;
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->listByUser(bad, &page));

    cm::db::ShareListQuery halfCursor = query;
    halfCursor.cursorCreatedAt = "2021-01-02 03:04:05.123456";
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->listByUser(halfCursor, &page));
}

TEST_F(ShareDaoTest, ShareDaoCancelIdempotent) {
    const std::uint64_t ufId = nextUfId();
    const std::string first = createFileShare(ufId);
    const std::string second = createFileShare(ufId);

    std::vector<std::string> active;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->listActiveByUfId(ufId, &active));
    ASSERT_EQ(std::size_t(2), active.size());

    bool cancelled = false;
    bool already = true;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->cancel(m_uid, first, &cancelled, &already));
    EXPECT_TRUE(cancelled);
    EXPECT_FALSE(already);

    cm::db::ShareRow row;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findByCode(first, &row));
    EXPECT_EQ(1, static_cast<int>(row.status));

    // 第二次取消：幂等，cancelled=false / already=true，状态不变。
    cancelled = true;
    already = false;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->cancel(m_uid, first, &cancelled, &already));
    EXPECT_FALSE(cancelled);
    EXPECT_TRUE(already);
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findByCode(first, &row));
    EXPECT_EQ(1, static_cast<int>(row.status));

    active.clear();
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->listActiveByUfId(ufId, &active));
    ASSERT_EQ(std::size_t(1), active.size());
    EXPECT_EQ(second, active[0]);

    // 非本人 / 不存在：NotFound。
    EXPECT_EQ(cm::ErrorCode::NotFound,
              m_dao->cancel(m_uid + 100000000ULL, second, &cancelled, &already));
    EXPECT_EQ(cm::ErrorCode::NotFound,
              m_dao->cancel(m_uid, nextCode(), &cancelled, &already));

    // 全部取消后没有活跃分享。
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->cancel(m_uid, second, &cancelled, &already));
    active.clear();
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->listActiveByUfId(ufId, &active));
    EXPECT_TRUE(active.empty());
}

TEST_F(ShareDaoTest, ShareDaoSweepExpired) {
    const std::string expiredA = createFileShare(m_ufId);
    const std::string expiredB = createFileShare(m_ufId);
    const std::string fresh = createFileShare(m_ufId);

    ASSERT_TRUE(execPreparedString(
        "UPDATE t_share SET s_expire_at = DATE_SUB(NOW(6), INTERVAL 1 HOUR) "
        "WHERE s_code = ?", expiredA));
    ASSERT_TRUE(execPreparedString(
        "UPDATE t_share SET s_expire_at = DATE_SUB(NOW(6), INTERVAL 1 HOUR) "
        "WHERE s_code = ?", expiredB));

    std::uint64_t affected = 0;
    // 全局扫描：测试库可能残留其他过期分享，只断言“至少两条”。
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->sweepExpired(200, &affected));
    EXPECT_GE(affected, std::uint64_t(2));

    cm::db::ShareRow row;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findByCode(expiredA, &row));
    EXPECT_EQ(1, static_cast<int>(row.status));
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findByCode(expiredB, &row));
    EXPECT_EQ(1, static_cast<int>(row.status));
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findByCode(fresh, &row));
    EXPECT_EQ(0, static_cast<int>(row.status));

    // 再扫一次：已取消的不会再被计入（幂等）。
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->sweepExpired(200, &affected));
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findByCode(fresh, &row));
    EXPECT_EQ(0, static_cast<int>(row.status));

    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->sweepExpired(0, &affected));
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->sweepExpired(1001, &affected));
}

}  // namespace
