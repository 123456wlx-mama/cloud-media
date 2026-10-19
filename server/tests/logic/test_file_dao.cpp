#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "dao_fixture.h"
#include "db/file_dao.h"

namespace {

class FileDaoTest : public cm::test::DaoTestBase {
protected:
    void SetUp() override {
        cm::test::DaoTestBase::SetUp();
        if (m_pool == NULL) {
            return;  // 基类已 GTEST_SKIP
        }
        m_dao.reset(new cm::db::FileDao(*m_conn));
        createUser();
    }

    // 保留原用例里的“名字 + 是否目录”写法；基类用 fileId==0 表示目录。
    std::uint64_t createNode(const std::string& name,
                             bool isDir,
                             std::uint64_t fid,
                             std::string* finalName) {
        return DaoTestBase::createNode(m_uid, name, isDir ? 0 : fid, finalName);
    }

    std::uint64_t createNode(const std::string& name, bool isDir, std::uint64_t fid) {
        return createNode(name, isDir, fid, NULL);
    }

    std::string nextMd5() { return nextHex32(); }


    std::vector<cm::db::UserFileRow> collectAllPages(cm::db::FileListQuery query,
                                                     std::size_t* pages) {
        std::vector<cm::db::UserFileRow> all;
        *pages = 0;
        for (;;) {
            cm::db::FileListPage page;
            const cm::ErrorCode rc = m_dao->listByParent(query, &page);
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
            query.cursorKey = page.nextKey;
            query.cursorId = page.nextId;
            if (all.size() > 1000) {
                ADD_FAILURE() << "pagination did not terminate";
                break;
            }
        }
        return all;
    }

    std::unique_ptr<cm::db::FileDao> m_dao;
};

TEST_F(FileDaoTest, FileDaoFindBlobByMd5) {
    cm::db::BlobRow missing;
    EXPECT_EQ(cm::ErrorCode::NotFound, m_dao->findBlobByMd5(nextMd5(), &missing));

    const std::string md5 = nextMd5();
    std::uint64_t fid = 0;
    cm::db::BlobWriteKind kind = cm::db::BlobWriteKind::Inserted;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->insertBlobOnDuplicateIncrement(
                  md5, 1234567, "aa/bb/" + md5, &fid, &kind));
    trackBlob(fid);

    cm::db::BlobRow row;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findBlobByMd5(md5, &row));
    EXPECT_EQ(fid, row.fid);
    EXPECT_EQ(md5, row.md5);
    EXPECT_EQ(std::uint64_t(1234567), row.size);
    EXPECT_EQ(std::string("aa/bb/") + md5, row.path);
    EXPECT_EQ(std::uint32_t(1), row.refCount);
    EXPECT_EQ(std::uint8_t(1), row.state);

    cm::db::BlobRow byId;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->getBlobById(fid, &byId));
    EXPECT_EQ(md5, byId.md5);
    EXPECT_EQ(row.path, byId.path);

    EXPECT_EQ(cm::ErrorCode::NotFound,
              m_dao->getBlobById(fid + 100000000ULL, &byId));
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->findBlobByMd5("", &row));
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->getBlobById(0, &byId));
}

TEST_F(FileDaoTest, FileDaoInsertBlobIncrementsRef) {
    const std::string md5 = nextMd5();
    const std::string path = "aa/bb/" + md5;

    std::uint64_t first = 0;
    cm::db::BlobWriteKind kind = cm::db::BlobWriteKind::RefIncremented;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->insertBlobOnDuplicateIncrement(md5, 4096, path, &first, &kind));
    EXPECT_EQ(cm::db::BlobWriteKind::Inserted, kind);
    ASSERT_GT(first, std::uint64_t(0));
    trackBlob(first);

    std::uint64_t second = 0;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->insertBlobOnDuplicateIncrement(
                  md5, 4096, "cc/dd/ignored", &second, &kind));
    EXPECT_EQ(cm::db::BlobWriteKind::RefIncremented, kind);
    EXPECT_EQ(first, second);

    cm::db::BlobRow row;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->getBlobById(first, &row));
    EXPECT_EQ(std::uint32_t(2), row.refCount);
    EXPECT_EQ(std::uint8_t(1), row.state);
    // 命中已有 blob 时不得覆盖路径。
    EXPECT_EQ(path, row.path);
}

TEST_F(FileDaoTest, FileDaoAutoRename) {
    const std::uint64_t fid = createBlob(2048);
    ASSERT_GT(fid, std::uint64_t(0));

    std::string name1;
    std::string name2;
    std::string name3;

    ASSERT_NE(std::uint64_t(0), createNode("report.txt", false, fid, &name1));
    ASSERT_NE(std::uint64_t(0), createNode("report.txt", false, fid, &name2));
    ASSERT_NE(std::uint64_t(0), createNode("report.txt", false, fid, &name3));
    EXPECT_EQ(std::string("report.txt"), name1);
    EXPECT_EQ(std::string("report (2).txt"), name2);
    EXPECT_EQ(std::string("report (3).txt"), name3);

    std::string noExt;
    ASSERT_NE(std::uint64_t(0), createNode("noext", false, fid, &noExt));
    std::string noExt2;
    ASSERT_NE(std::uint64_t(0), createNode("noext", false, fid, &noExt2));
    EXPECT_EQ(std::string("noext (2)"), noExt2);

    // 目录：f_id 必须为 NULL，isDir 为真。
    const std::uint64_t dirId = createNode("photos", true, 0);
    ASSERT_NE(std::uint64_t(0), dirId);
    cm::db::UserFileRow dirRow;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findUserFile(m_uid, dirId, &dirRow));
    EXPECT_TRUE(dirRow.isDir);
    EXPECT_EQ(std::uint64_t(0), dirRow.fid);
    EXPECT_EQ(std::string("photos"), dirRow.name);

    // 参数守卫：目录带 f_id、文件缺 f_id 都拒绝。
    std::uint64_t ignored = 0;
    std::string ignoredName;
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              m_dao->createUserFileAutoRename(
                  m_uid, 0, fid, true, "bad_dir", &ignored, &ignoredName));
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              m_dao->createUserFileAutoRename(
                  m_uid, 0, 0, false, "bad_file", &ignored, &ignoredName));
}

TEST_F(FileDaoTest, FileDaoSoftDeleteDecrementsOnce) {
    const std::uint64_t fid = createBlob(4096);
    ASSERT_GT(fid, std::uint64_t(0));
    const std::uint64_t ufId = createNode("del.bin", false, fid);
    ASSERT_GT(ufId, std::uint64_t(0));

    bool already = true;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->softDelete(m_uid, ufId, &already));
    EXPECT_FALSE(already);

    cm::db::BlobRow row;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->getBlobById(fid, &row));
    EXPECT_EQ(std::uint32_t(0), row.refCount);
    EXPECT_EQ(std::uint8_t(2), row.state);

    // 重复删除：幂等，不再减计数（否则会无符号下溢）。
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->softDelete(m_uid, ufId, &already));
    EXPECT_TRUE(already);
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->getBlobById(fid, &row));
    EXPECT_EQ(std::uint32_t(0), row.refCount);

    cm::db::UserFileRow node;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findUserFile(m_uid, ufId, &node));
    EXPECT_NE(std::string(cm::db::FileDao::activeDeletedAt()), node.deletedAt);

    // 目录没有 f_id，删除不得影响任何 blob 计数。
    const std::uint64_t untouched = createBlob(512);
    const std::uint64_t dirId = createNode("dir_del", true, 0);
    ASSERT_GT(dirId, std::uint64_t(0));
    bool dirAlready = true;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->softDelete(m_uid, dirId, &dirAlready));
    EXPECT_FALSE(dirAlready);
    cm::db::BlobRow untouchedRow;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->getBlobById(untouched, &untouchedRow));
    EXPECT_EQ(std::uint32_t(1), untouchedRow.refCount);
    EXPECT_EQ(std::uint8_t(1), untouchedRow.state);

    // 非本人节点：NotFound，不泄露存在性。
    EXPECT_EQ(cm::ErrorCode::NotFound,
              m_dao->softDelete(m_uid + 100000000ULL, ufId, &already));
}

TEST_F(FileDaoTest, FileDaoRestoreIncrementsOnce) {
    const std::uint64_t fid = createBlob(8192);
    const std::uint64_t ufId = createNode("restore.bin", false, fid);
    ASSERT_GT(ufId, std::uint64_t(0));

    bool already = false;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->softDelete(m_uid, ufId, &already));
    ASSERT_FALSE(already);

    cm::db::RestoreOutcome outcome;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->restore(m_uid, ufId, 0, "restore.bin", &outcome));
    EXPECT_FALSE(outcome.alreadyRestored);
    EXPECT_EQ(fid, outcome.fid);
    EXPECT_EQ(std::string("restore.bin"), outcome.name);

    cm::db::BlobRow row;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->getBlobById(fid, &row));
    EXPECT_EQ(std::uint32_t(1), row.refCount);
    EXPECT_EQ(std::uint8_t(1), row.state);

    cm::db::UserFileRow node;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findUserFile(m_uid, ufId, &node));
    EXPECT_EQ(std::string(cm::db::FileDao::activeDeletedAt()), node.deletedAt);

    // 已恢复：幂等，不再 +1。
    outcome = cm::db::RestoreOutcome();
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->restore(m_uid, ufId, 0, "restore.bin", &outcome));
    EXPECT_TRUE(outcome.alreadyRestored);
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->getBlobById(fid, &row));
    EXPECT_EQ(std::uint32_t(1), row.refCount);

    // 恢复时同名冲突：自动改名，避免 1062 冒到服务层。
    const std::uint64_t fid2 = createBlob(1024);
    const std::uint64_t conflictId = createNode("conflict.bin", false, fid2);
    ASSERT_GT(conflictId, std::uint64_t(0));
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->softDelete(m_uid, conflictId, &already));
    ASSERT_GT(createNode("conflict.bin", false, fid2), std::uint64_t(0));

    cm::db::RestoreOutcome renamed;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->restore(m_uid, conflictId, 0, "conflict.bin", &renamed));
    EXPECT_EQ(std::string("conflict (2).bin"), renamed.name);
}

TEST_F(FileDaoTest, FileDaoListNameCursor) {
    const std::uint64_t fid = createBlob(4096);
    std::vector<std::string> expected;
    for (int i = 1; i <= 12; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "n%02d", i);
        expected.push_back(std::string(name));
        ASSERT_GT(createNode(name, false, fid), std::uint64_t(0));
    }

    std::uint64_t total = 0;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->countByParent(m_uid, 0, &total));
    EXPECT_EQ(std::uint64_t(12), total);

    cm::db::FileListQuery query;
    query.uid = m_uid;
    query.parentId = 0;
    query.sort = cm::db::FileListSort::NameAsc;
    query.limit = 5;

    std::size_t pages = 0;
    const std::vector<cm::db::UserFileRow> rows = collectAllPages(query, &pages);
    ASSERT_EQ(std::size_t(12), rows.size());
    EXPECT_EQ(std::size_t(3), pages);

    std::set<std::uint64_t> seen;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        EXPECT_TRUE(seen.insert(rows[i].ufId).second) << "duplicate uf_id";
        EXPECT_EQ(expected[i], rows[i].name);
        if (i > 0) {
            EXPECT_LT(rows[i - 1].ufId, rows[i].ufId) << "cursor order broken";
        }
    }

    // 分页参数校验：limit 越界、游标半空都必须拒绝。
    cm::db::FileListPage page;
    cm::db::FileListQuery bad = query;
    bad.limit = 0;
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->listByParent(bad, &page));
    bad.limit = 201;
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->listByParent(bad, &page));

    cm::db::FileListQuery halfCursor = query;
    halfCursor.cursorKey = "n05";
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              m_dao->listByParent(halfCursor, &page));
}

TEST_F(FileDaoTest, FileDaoListSameMicrosecond) {
    const std::uint64_t fid = createBlob(4096);
    for (int i = 1; i <= 7; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "s%02d", i);
        ASSERT_GT(createNode(name, false, fid), std::uint64_t(0));
    }

    // 批量插入常常落在同一微秒：强制成同一时间戳，验证 (time, uf_id) 游标仍稳定。
    const std::string sameTime = "2021-01-02 03:04:05.123456";
    ASSERT_TRUE(execPreparedString(
        "UPDATE t_user_file SET uf_created_at = ? WHERE u_id = ?", sameTime, m_uid));

    cm::db::FileListQuery query;
    query.uid = m_uid;
    query.parentId = 0;
    query.sort = cm::db::FileListSort::CreatedDesc;
    query.limit = 3;

    std::size_t pages = 0;
    const std::vector<cm::db::UserFileRow> rows = collectAllPages(query, &pages);
    ASSERT_EQ(std::size_t(7), rows.size());
    EXPECT_EQ(std::size_t(3), pages);

    std::set<std::uint64_t> seen;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        EXPECT_TRUE(seen.insert(rows[i].ufId).second) << "duplicate uf_id";
        EXPECT_EQ(sameTime, rows[i].createdAt);
        if (i > 0) {
            // 时间相同，只能靠 uf_id 降序 tie-break。
            EXPECT_GT(rows[i - 1].ufId, rows[i].ufId);
        }
    }
}

}  // namespace

// 静态检查：列表 SQL 必须走行构造器游标，不能出现偏移量分页。
TEST(FileDaoStaticTest, FileDaoNoOffsetSql) {
#ifndef CM_SOURCE_DIR
    FAIL() << "CM_SOURCE_DIR is not defined";
#else
    const std::string path =
        std::string(CM_SOURCE_DIR) + "/server/db/file_dao.cpp";
    std::ifstream source(path.c_str());
    ASSERT_TRUE(source.is_open()) << "cannot open " << path;

    std::ostringstream buffer;
    buffer << source.rdbuf();
    const std::string text = buffer.str();

    EXPECT_EQ(std::string::npos, text.find("OFFSET"))
        << "file_dao.cpp must not use offset pagination";
    EXPECT_EQ(std::string::npos, text.find("LIMIT ?, ?"))
        << "file_dao.cpp must not bind a limit/offset pair";
    EXPECT_NE(std::string::npos, text.find("(uf_name, uf_id) > (?, ?)"));
    EXPECT_NE(std::string::npos, text.find("(uf_created_at, uf_id) < (?, ?)"));
    EXPECT_NE(std::string::npos, text.find("(uf_updated_at, uf_id) < (?, ?)"));
#endif
}
