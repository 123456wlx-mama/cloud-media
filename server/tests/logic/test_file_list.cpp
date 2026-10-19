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
#include "db/prepared_statement.h"
#include "logic/file_service.h"

namespace {

class FileListTest : public cm::test::DaoTestBase {
protected:
    void SetUp() override {
        cm::test::DaoTestBase::SetUp();
        if (m_pool == NULL) {
            return;  // 基类已 GTEST_SKIP（缺 MySQL）
        }
        m_service.reset(new cm::logic::FileService(*m_pool));
        createUser();
    }

    void TearDown() override {
        m_service.reset();
        cm::test::DaoTestBase::TearDown();
    }

    // 与 test_file_dao 一致：fileId==0 表示目录。
    std::uint64_t createNode(const std::string& name, bool isDir,
                             std::uint64_t fid) {
        return DaoTestBase::createNode(m_uid, name, isDir ? 0 : fid, NULL);
    }

    // 批量直插：1000 行用例避免 1000 次事务。清理由 cleanup 按 u_id 兜底。
    void bulkInsert(const std::uint64_t fid, const std::string& prefix,
                    int count) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "INSERT INTO t_user_file (u_id, uf_parent_id, f_id, uf_name) "
            "VALUES (?, ?, ?, ?)");
        ASSERT_TRUE(stmt) << m_conn->lastErrorMessage();
        for (int i = 1; i <= count; ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "%s%04d", prefix.c_str(), i);
            ASSERT_TRUE(stmt->bindUInt64(0, m_uid));
            ASSERT_TRUE(stmt->bindUInt64(1, 0));
            ASSERT_TRUE(stmt->bindUInt64(2, fid));
            ASSERT_TRUE(stmt->bindString(3, std::string(name)));
            ASSERT_TRUE(stmt->execute()) << stmt->lastErrorMessage();
        }
    }

    cm::protocol::ListFileRq makeRq(std::uint8_t sort, std::uint32_t limit) {
        cm::protocol::ListFileRq rq;
        rq.parentId = 0;
        rq.sort = sort;
        rq.limit = limit;
        return rq;
    }

    std::size_t collectPages(cm::logic::FileService& service,
                             std::uint8_t sort, std::uint32_t limit,
                             std::vector<cm::protocol::FileItem>* items,
                             std::uint64_t* firstTotal) {
        items->clear();
        *firstTotal = 0;
        cm::protocol::ListFileRq rq = makeRq(sort, limit);
        std::size_t pages = 0;
        for (;;) {
            cm::protocol::ListFileRs rs;
            const cm::ErrorCode rc = service.list(m_uid, rq, &rs);
            EXPECT_EQ(cm::ErrorCode::Ok, rc);
            if (rc != cm::ErrorCode::Ok) {
                return pages;
            }
            if (pages == 0) {
                *firstTotal = rs.totalCount;
            }
            for (std::size_t i = 0; i < rs.items.size(); ++i) {
                items->push_back(rs.items[i]);
            }
            ++pages;
            if (!rs.hasMore) {
                return pages;
            }
            rq.cursorKey = rs.nextKey;
            rq.cursorId = rs.nextId;
            if (pages > 10000) {
                ADD_FAILURE() << "pagination does not terminate";
                return pages;
            }
        }
    }

    std::unique_ptr<cm::logic::FileService> m_service;
};

TEST_F(FileListTest, FileListNameAscendingStable) {
    const std::uint64_t fid = createBlob(4096);
    bulkInsert(fid, "n", 12);
    ASSERT_GT(createNode("zdir", true, 0), std::uint64_t(0));

    std::vector<cm::protocol::FileItem> items;
    std::uint64_t total = 0;
    const std::size_t pages =
        collectPages(*m_service, 0, 5, &items, &total);
    ASSERT_EQ(std::size_t(13), items.size());
    EXPECT_EQ(std::size_t(3), pages);
    EXPECT_EQ(std::uint64_t(13), total);

    cm::db::BlobRow blob;
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::db::FileDao(*m_conn).getBlobById(fid, &blob));

    std::set<std::uint64_t> seen;
    for (std::size_t i = 0; i < items.size(); ++i) {
        EXPECT_TRUE(seen.insert(items[i].ufId).second) << "duplicate uf_id";
        if (i > 0) {
            EXPECT_LT(items[i - 1].name, items[i].name) << "order broken";
        }
        if (items[i].isDir) {
            EXPECT_EQ(std::uint64_t(0), items[i].size);
            EXPECT_TRUE(items[i].md5.empty());
        } else {
            EXPECT_EQ(fid, items[i].fId);
            EXPECT_EQ(std::uint64_t(4096), items[i].size);
            EXPECT_EQ(blob.md5, items[i].md5);
        }
        EXPECT_GT(items[i].createdAtMs, std::uint64_t(0));
    }
}

TEST_F(FileListTest, FileListCreatedDescSameMicrosecond) {
    const std::uint64_t fid = createBlob(1024);
    bulkInsert(fid, "c", 7);

    const std::string sameTime = "2021-01-02 03:04:05.123456";
    ASSERT_TRUE(execPreparedString(
        "UPDATE t_user_file SET uf_created_at = ? WHERE u_id = ?", sameTime,
        m_uid));

    std::vector<cm::protocol::FileItem> items;
    std::uint64_t total = 0;
    const std::size_t pages =
        collectPages(*m_service, 1, 3, &items, &total);
    ASSERT_EQ(std::size_t(7), items.size());
    EXPECT_EQ(std::size_t(3), pages);
    EXPECT_EQ(std::uint64_t(7), total);

    std::set<std::uint64_t> seen;
    for (std::size_t i = 0; i < items.size(); ++i) {
        EXPECT_TRUE(seen.insert(items[i].ufId).second) << "duplicate uf_id";
        // 同一微秒只能靠 uf_id DESC 兜底。
        if (i > 0) {
            EXPECT_GT(items[i - 1].ufId, items[i].ufId);
        }
        EXPECT_EQ(std::uint64_t(1609556645123ULL), items[i].createdAtMs);
    }
}

TEST_F(FileListTest, FileListUpdatedDesc) {
    const std::uint64_t fid = createBlob(2048);
    std::vector<std::uint64_t> ids;
    for (int i = 1; i <= 5; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "u%d", i);
        const std::uint64_t id = createNode(name, false, fid);
        ASSERT_GT(id, std::uint64_t(0));
        ids.push_back(id);
        char time[40];
        // id 越大越新：u5 最新，应排在最前。
        std::snprintf(time, sizeof(time), "2021-01-02 03:04:%02d.000000", i);
        ASSERT_TRUE(execPreparedString(
            "UPDATE t_user_file SET uf_updated_at = ? WHERE uf_id = ?",
            std::string(time), id));
    }

    std::vector<cm::protocol::FileItem> items;
    std::uint64_t total = 0;
    const std::size_t pages =
        collectPages(*m_service, 2, 2, &items, &total);
    ASSERT_EQ(std::size_t(5), items.size());
    EXPECT_EQ(std::size_t(3), pages);
    EXPECT_EQ(std::uint64_t(5), total);
    for (std::size_t i = 0; i < items.size(); ++i) {
        char expected[16];
        std::snprintf(expected, sizeof(expected), "u%d",
                      static_cast<int>(5 - i));
        EXPECT_EQ(std::string(expected), items[i].name);
        // 2021-01-02 03:04:0i UTC。
        EXPECT_EQ(std::uint64_t(1609556640000ULL) + (5 - i) * 1000ULL,
                  items[i].updatedAtMs);
    }
}

TEST_F(FileListTest, FileListHasMore) {
    const std::uint64_t fid = createBlob(512);
    bulkInsert(fid, "h", 5);

    cm::protocol::ListFileRq rq = makeRq(0, 2);
    cm::protocol::ListFileRs page1;
    ASSERT_EQ(cm::ErrorCode::Ok, m_service->list(m_uid, rq, &page1));
    ASSERT_EQ(std::size_t(2), page1.items.size());
    EXPECT_EQ(std::uint8_t(1), page1.hasMore);
    EXPECT_EQ(page1.items.back().name, page1.nextKey);
    EXPECT_EQ(page1.items.back().ufId, page1.nextId);
    EXPECT_EQ(std::uint64_t(5), page1.totalCount);

    rq.cursorKey = page1.nextKey;
    rq.cursorId = page1.nextId;
    cm::protocol::ListFileRs page2;
    ASSERT_EQ(cm::ErrorCode::Ok, m_service->list(m_uid, rq, &page2));
    ASSERT_EQ(std::size_t(2), page2.items.size());
    EXPECT_EQ(std::uint8_t(1), page2.hasMore);
    // 非首页不再统计。
    EXPECT_EQ(cm::logic::FileService::kUnknownTotalCount, page2.totalCount);

    rq.cursorKey = page2.nextKey;
    rq.cursorId = page2.nextId;
    cm::protocol::ListFileRs page3;
    ASSERT_EQ(cm::ErrorCode::Ok, m_service->list(m_uid, rq, &page3));
    ASSERT_EQ(std::size_t(1), page3.items.size());
    EXPECT_EQ(std::uint8_t(0), page3.hasMore);
    EXPECT_EQ(cm::logic::FileService::kUnknownTotalCount, page3.totalCount);
}

TEST_F(FileListTest, FileListInvalidCursor) {
    const std::string validTime = "2026-10-19 12:00:00.123456";
    const std::string longName(256, 'n');

    struct Case {
        std::uint8_t sort;
        std::string key;
        std::uint64_t id;
        bool ok;
    };
    const Case cases[] = {
        {3, "", 0, false},                       // sort 越界
        {0, "n0001", 0, false},                  // 半空游标
        {0, "", 5, false},                       // 半空游标
        {0, longName, 1, false},                 // 名称 >255 字节
        {1, "2026-10-19", 1, false},             // 非 DATETIME(6)
        {1, "2026-10-19 12:00:00", 1, false},    // 缺微秒
        {2, "2026-10-19 12:00:00.12345x", 1, false},
        {1, validTime, 1, true},
        {2, validTime, 1, true},
        {0, "n0001", 1, true},
    };

    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        cm::protocol::ListFileRq rq = makeRq(cases[i].sort, 10);
        rq.cursorKey = cases[i].key;
        rq.cursorId = cases[i].id;
        cm::protocol::ListFileRs rs;
        const cm::ErrorCode rc = m_service->list(m_uid, rq, &rs);
        if (cases[i].ok) {
            EXPECT_EQ(cm::ErrorCode::Ok, rc) << "case " << i;
        } else {
            EXPECT_EQ(cm::ErrorCode::InvalidArgument, rc) << "case " << i;
            EXPECT_EQ(std::uint32_t(1001), rs.code) << "case " << i;
        }
    }
}

TEST_F(FileListTest, FileListTotalCountCap) {
    const std::uint64_t fid = createBlob(256);
    bulkInsert(fid, "t", 4);

    cm::logic::FileService capped(*m_pool, 3);
    cm::protocol::ListFileRq rq = makeRq(0, 50);
    cm::protocol::ListFileRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok, capped.list(m_uid, rq, &rs));
    EXPECT_EQ(std::size_t(4), rs.items.size());
    EXPECT_EQ(std::uint64_t(3), rs.totalCount);  // 超过上限只报上限
    EXPECT_EQ(std::uint8_t(0), rs.hasMore);

#ifndef CM_SOURCE_DIR
    FAIL() << "CM_SOURCE_DIR is not defined";
#else
    // 超时降级靠这两处：200ms 提示 + LIMIT 10001 截断。
    const std::string path =
        std::string(CM_SOURCE_DIR) + "/server/db/file_dao.cpp";
    std::ifstream source(path.c_str());
    ASSERT_TRUE(source.is_open()) << "cannot open " << path;
    std::ostringstream buffer;
    buffer << source.rdbuf();
    const std::string text = buffer.str();
    EXPECT_NE(std::string::npos, text.find("MAX_EXECUTION_TIME(200)"));
    EXPECT_NE(std::string::npos, text.find("LIMIT 10001"));
#endif
}

TEST_F(FileListTest, FileListPaging1000) {
    const std::uint64_t fid = createBlob(128);
    bulkInsert(fid, "f", 1000);

    std::vector<cm::protocol::FileItem> items;
    std::uint64_t total = 0;
    const std::size_t pages =
        collectPages(*m_service, 0, 50, &items, &total);
    ASSERT_EQ(std::size_t(1000), items.size());
    EXPECT_EQ(std::size_t(20), pages);
    EXPECT_EQ(std::uint64_t(1000), total);

    std::set<std::uint64_t> seen;
    for (std::size_t i = 0; i < items.size(); ++i) {
        EXPECT_TRUE(seen.insert(items[i].ufId).second) << "duplicate uf_id";
        char expected[16];
        std::snprintf(expected, sizeof(expected), "f%04d",
                      static_cast<int>(i + 1));
        ASSERT_EQ(std::string(expected), items[i].name);
    }
}

TEST_F(FileListTest, FileListSameMicrosecondBatch) {
    const std::uint64_t fid = createBlob(64);
    bulkInsert(fid, "m", 25);

    const std::string sameTime = "2022-02-03 04:05:06.654321";
    ASSERT_TRUE(execPreparedString(
        "UPDATE t_user_file SET uf_updated_at = ? WHERE u_id = ?", sameTime,
        m_uid));

    std::vector<cm::protocol::FileItem> items;
    std::uint64_t total = 0;
    const std::size_t pages =
        collectPages(*m_service, 2, 7, &items, &total);
    ASSERT_EQ(std::size_t(25), items.size());
    EXPECT_EQ(std::size_t(4), pages);
    EXPECT_EQ(std::uint64_t(25), total);

    std::set<std::uint64_t> seen;
    for (std::size_t i = 0; i < items.size(); ++i) {
        EXPECT_TRUE(seen.insert(items[i].ufId).second) << "duplicate uf_id";
        if (i > 0) {
            EXPECT_GT(items[i - 1].ufId, items[i].ufId);
        }
        EXPECT_EQ(std::uint64_t(1643861106654ULL), items[i].updatedAtMs);
    }
}

TEST_F(FileListTest, FileListLimitBounds) {
    const std::uint64_t fid = createBlob(32);
    bulkInsert(fid, "l", 60);

    cm::protocol::ListFileRq rq = makeRq(0, 0);  // 0 = 默认 50
    cm::protocol::ListFileRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok, m_service->list(m_uid, rq, &rs));
    EXPECT_EQ(std::size_t(50), rs.items.size());
    EXPECT_EQ(std::uint8_t(1), rs.hasMore);
    EXPECT_EQ(std::uint64_t(60), rs.totalCount);

    rq = makeRq(0, 1);
    ASSERT_EQ(cm::ErrorCode::Ok, m_service->list(m_uid, rq, &rs));
    EXPECT_EQ(std::size_t(1), rs.items.size());
    EXPECT_EQ(std::uint8_t(1), rs.hasMore);

    rq = makeRq(0, 200);
    ASSERT_EQ(cm::ErrorCode::Ok, m_service->list(m_uid, rq, &rs));
    EXPECT_EQ(std::size_t(60), rs.items.size());
    EXPECT_EQ(std::uint8_t(0), rs.hasMore);

    rq = makeRq(0, 201);
    const cm::ErrorCode rc = m_service->list(m_uid, rq, &rs);
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, rc);
    EXPECT_EQ(std::uint32_t(1001), rs.code);
}

// 静态检查：服务层与 DAO 都不能回退到 OFFSET 分页。
TEST(FileListStaticTest, FileListNoOffsetSql) {
#ifndef CM_SOURCE_DIR
    FAIL() << "CM_SOURCE_DIR is not defined";
#else
    const char* files[] = {"/server/logic/file_service.cpp",
                           "/server/db/file_dao.cpp"};
    for (std::size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        const std::string path = std::string(CM_SOURCE_DIR) + files[i];
        std::ifstream source(path.c_str());
        ASSERT_TRUE(source.is_open()) << "cannot open " << path;
        std::ostringstream buffer;
        buffer << source.rdbuf();
        const std::string text = buffer.str();
        EXPECT_EQ(std::string::npos, text.find("OFFSET"))
            << files[i] << " must not use offset pagination";
        EXPECT_EQ(std::string::npos, text.find("LIMIT ?, ?"))
            << files[i] << " must not bind a limit/offset pair";
    }
#endif
}

}  // namespace
