#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "cache/cache_store.h"
#include "cache/redis_pool.h"
#include "dao_fixture.h"
#include "db/file_dao.h"
#include "logic/recycle_service.h"

namespace {

bool redisEnabled() {
    const char* value = std::getenv("CM_TEST_REDIS");
    return value != nullptr && std::string(value) == "1";
}

std::string envOr(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string(fallback);
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

class RecycleServiceTest : public cm::test::DaoTestBase {
protected:
    void SetUp() override {
        cm::test::DaoTestBase::SetUp();
        if (m_pool == nullptr) {
            return;  // 基类已 GTEST_SKIP（缺 MySQL）
        }
        if (!redisEnabled()) {
            GTEST_SKIP() << "set CM_TEST_REDIS=1 (db 15) to run RecycleService tests";
            return;
        }

        std::string error;
        m_redis = cm::cache::RedisPool::create(redisOptions(), &error);
        ASSERT_TRUE(m_redis) << error;
        m_cache.reset(new cm::cache::CacheStore(*m_redis, cm::cache::CacheStoreOptions()));
        m_service.reset(new cm::logic::RecycleService(*m_pool, *m_cache));
        createUser();
    }

    void TearDown() override {
        m_service.reset();
        m_cache.reset();
        if (m_redis && !m_redisDown) {
            m_redis->shutdown();
        }
        m_redis.reset();
        cm::test::DaoTestBase::TearDown();
    }

    cm::protocol::DeleteFileRq makeDeleteRq(std::uint64_t ufId) {
        cm::protocol::DeleteFileRq rq;
        rq.ufId = ufId;
        return rq;
    }

    cm::protocol::RestoreRq makeRestoreRq(std::uint64_t ufId,
                                          const std::string& newName) {
        cm::protocol::RestoreRq rq;
        rq.ufId = ufId;
        rq.newName = newName;
        rq.autoRename = 1;
        return rq;
    }

    cm::protocol::PurgeRq makePurgeRq(std::uint64_t ufId) {
        cm::protocol::PurgeRq rq;
        rq.ufId = ufId;
        rq.confirm = 1;
        return rq;
    }

    cm::protocol::RecycleListRq makeListRq(std::uint32_t limit,
                                           std::uint64_t cursorMs,
                                           std::uint64_t cursorId) {
        cm::protocol::RecycleListRq rq;
        rq.limit = limit;
        rq.cursorDeletedMs = cursorMs;
        rq.cursorId = cursorId;
        return rq;
    }

    cm::db::BlobRow loadBlob(std::uint64_t fid) {
        cm::db::BlobRow blob;
        cm::db::FileDao dao(*m_conn);
        EXPECT_EQ(cm::ErrorCode::Ok, dao.getBlobById(fid, &blob));
        return blob;
    }

    // 在指定 parentId 下建节点（fixture 的 createNode 固定根目录）。
    std::uint64_t createChild(std::uint64_t parentId,
                              const std::string& name,
                              std::uint64_t fid,
                              bool isDir) {
        cm::db::FileDao dao(*m_conn);
        std::uint64_t ufId = 0;
        std::string resolved;
        if (dao.createUserFileAutoRename(m_uid, parentId, fid, isDir, name,
                                         &ufId, &resolved) != cm::ErrorCode::Ok) {
            return 0;
        }
        trackNode(ufId);
        return ufId;
    }

    std::shared_ptr<cm::cache::RedisPool> m_redis;
    std::unique_ptr<cm::cache::CacheStore> m_cache;
    std::unique_ptr<cm::logic::RecycleService> m_service;
    bool m_redisDown = false;
};

TEST_F(RecycleServiceTest, DeleteDecrementsOnce) {
    const std::uint64_t fid = createBlob(1024);
    ASSERT_GT(fid, std::uint64_t(0));
    const std::uint64_t ufId = createNode(m_uid, "del.bin", fid);
    ASSERT_GT(ufId, std::uint64_t(0));
    ASSERT_EQ(std::uint32_t(1), loadBlob(fid).refCount);

    cm::protocol::DeleteFileRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->deleteFile(m_uid, makeDeleteRq(ufId), &rs));
    EXPECT_EQ(0u, rs.code);
    EXPECT_EQ(1, static_cast<int>(rs.deleted));
    EXPECT_EQ(0, static_cast<int>(rs.already));

    const cm::db::BlobRow after = loadBlob(fid);
    EXPECT_EQ(std::uint32_t(0), after.refCount);
    EXPECT_EQ(2, static_cast<int>(after.state));  // 减到 0 置孤儿

    // 重复删除：already=1，不二次减（不下溢）。
    cm::protocol::DeleteFileRs again;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->deleteFile(m_uid, makeDeleteRq(ufId), &again));
    EXPECT_EQ(0, static_cast<int>(again.deleted));
    EXPECT_EQ(1, static_cast<int>(again.already));
    EXPECT_EQ(std::uint32_t(0), loadBlob(fid).refCount);
}

TEST_F(RecycleServiceTest, RestoreIncrementsOnce) {
    const std::uint64_t fid = createBlob(1024);
    const std::uint64_t ufId = createNode(m_uid, "res.bin", fid);
    cm::protocol::DeleteFileRs drs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->deleteFile(m_uid, makeDeleteRq(ufId), &drs));
    ASSERT_EQ(std::uint32_t(0), loadBlob(fid).refCount);

    cm::protocol::RestoreRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->restore(m_uid, makeRestoreRq(ufId, ""), &rs));
    EXPECT_EQ(0u, rs.code);
    EXPECT_EQ(ufId, rs.ufId);
    EXPECT_EQ(std::uint64_t(0), rs.parentId);
    EXPECT_EQ(std::string("res.bin"), rs.name);
    EXPECT_EQ(0, static_cast<int>(rs.renamed));

    const cm::db::BlobRow after = loadBlob(fid);
    EXPECT_EQ(std::uint32_t(1), after.refCount);  // 恢复 +1
    EXPECT_EQ(1, static_cast<int>(after.state));  // 2→1

    // 再次恢复：alreadyRestored，不二次 +1。
    cm::protocol::RestoreRs again;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->restore(m_uid, makeRestoreRq(ufId, ""), &again));
    EXPECT_EQ(0u, again.code);
    EXPECT_EQ(0, static_cast<int>(again.renamed));
    EXPECT_EQ(std::uint32_t(1), loadBlob(fid).refCount);
}

// 删文件夹：只软删这一个目录节点，子节点保持可用（不级联、不动引用计数）。
// 用户视角与真网盘一致：回收站里只有一条"文件夹"，恢复后里面的内容都在。
TEST_F(RecycleServiceTest, DeleteDirectoryKeepsChildren) {
    const std::uint64_t fid = createBlob(1024);
    ASSERT_GT(fid, std::uint64_t(0));
    ASSERT_EQ(std::uint32_t(1), loadBlob(fid).refCount);

    const std::uint64_t dirId = createChild(0, "dir_del", 0, true);
    ASSERT_GT(dirId, std::uint64_t(0));
    const std::uint64_t childId = createChild(dirId, "inside.bin", fid, false);
    ASSERT_GT(childId, std::uint64_t(0));

    cm::protocol::DeleteFileRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->deleteFile(m_uid, makeDeleteRq(dirId), &rs));
    EXPECT_EQ(0u, rs.code);
    EXPECT_EQ(1, static_cast<int>(rs.deleted));

    // 目录已软删；子节点仍是 active（不级联）
    cm::db::FileDao dao(*m_conn);
    cm::db::UserFileRow node;
    ASSERT_EQ(cm::ErrorCode::Ok, dao.findUserFile(m_uid, dirId, &node));
    EXPECT_NE(cm::db::FileDao::activeDeletedAt(), node.deletedAt);
    ASSERT_EQ(cm::ErrorCode::Ok, dao.findUserFile(m_uid, childId, &node));
    EXPECT_EQ(cm::db::FileDao::activeDeletedAt(), node.deletedAt);

    // 目录没有 f_id，减不了计数；子节点还在引用，所以计数不变
    EXPECT_EQ(std::uint32_t(1), loadBlob(fid).refCount);

    // 回收站里只出现目录这一条，子节点不单独出现
    cm::protocol::RecycleListRs listRs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->listRecycle(m_uid, makeListRq(50, 0, 0), &listRs));
    bool hasDir = false;
    bool hasChild = false;
    for (std::size_t i = 0; i < listRs.items.size(); ++i) {
        if (listRs.items[i].ufId == dirId) {
            hasDir = true;
            EXPECT_EQ(1, static_cast<int>(listRs.items[i].isDir));
        }
        if (listRs.items[i].ufId == childId) {
            hasChild = true;
        }
    }
    EXPECT_TRUE(hasDir);
    EXPECT_FALSE(hasChild);

    // 恢复目录后子树回来（子节点一直是 active，只是父目录不可达）
    cm::protocol::RestoreRs restoreRs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->restore(m_uid, makeRestoreRq(dirId, ""), &restoreRs));
    EXPECT_EQ(0u, restoreRs.code);
    EXPECT_EQ(dirId, restoreRs.ufId);
    ASSERT_EQ(cm::ErrorCode::Ok, dao.findUserFile(m_uid, childId, &node));
    EXPECT_EQ(cm::db::FileDao::activeDeletedAt(), node.deletedAt);
    EXPECT_EQ(std::uint32_t(1), loadBlob(fid).refCount);
}

TEST_F(RecycleServiceTest, PurgeFileNoDoubleDecrement) {
    const std::uint64_t fid = createBlob(1024);
    const std::uint64_t ufId = createNode(m_uid, "purge.bin", fid);
    cm::protocol::DeleteFileRs drs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->deleteFile(m_uid, makeDeleteRq(ufId), &drs));
    ASSERT_EQ(std::uint32_t(0), loadBlob(fid).refCount);

    cm::protocol::PurgeRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok, m_service->purge(m_uid, makePurgeRq(ufId), &rs));
    EXPECT_EQ(0u, rs.code);
    EXPECT_EQ(1u, rs.filesDeleted);
    EXPECT_EQ(0u, rs.dirsDeleted);
    EXPECT_EQ(0u, rs.blobsOrphaned);  // 软删时已减，purge 不二次减

    cm::db::FileDao dao(*m_conn);
    cm::db::UserFileRow node;
    EXPECT_EQ(cm::ErrorCode::NotFound, dao.findUserFile(m_uid, ufId, &node));
    EXPECT_EQ(std::uint32_t(0), loadBlob(fid).refCount);
}

TEST_F(RecycleServiceTest, PurgeTreeActiveDescendants) {
    const std::uint64_t dirId = createChild(0, "dir", 0, true);
    ASSERT_GT(dirId, std::uint64_t(0));

    const std::uint64_t fid1 = createBlob(1024);
    const std::uint64_t fid2 = createBlob(2048);
    const std::uint64_t child1 = createChild(dirId, "a.bin", fid1, false);
    const std::uint64_t child2 = createChild(dirId, "b.bin", fid2, false);
    ASSERT_GT(child1, std::uint64_t(0));
    ASSERT_GT(child2, std::uint64_t(0));
    ASSERT_EQ(std::uint32_t(1), loadBlob(fid1).refCount);
    ASSERT_EQ(std::uint32_t(1), loadBlob(fid2).refCount);

    cm::protocol::PurgeRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok, m_service->purge(m_uid, makePurgeRq(dirId), &rs));
    EXPECT_EQ(0u, rs.code);
    EXPECT_EQ(2u, rs.filesDeleted);
    EXPECT_EQ(1u, rs.dirsDeleted);
    EXPECT_EQ(2u, rs.blobsOrphaned);  // 两个活跃文件补减到 0

    cm::db::FileDao dao(*m_conn);
    cm::db::UserFileRow node;
    EXPECT_EQ(cm::ErrorCode::NotFound, dao.findUserFile(m_uid, dirId, &node));
    EXPECT_EQ(cm::ErrorCode::NotFound, dao.findUserFile(m_uid, child1, &node));
    EXPECT_EQ(cm::ErrorCode::NotFound, dao.findUserFile(m_uid, child2, &node));

    const cm::db::BlobRow b1 = loadBlob(fid1);
    const cm::db::BlobRow b2 = loadBlob(fid2);
    EXPECT_EQ(std::uint32_t(0), b1.refCount);
    EXPECT_EQ(2, static_cast<int>(b1.state));
    EXPECT_EQ(std::uint32_t(0), b2.refCount);
    EXPECT_EQ(2, static_cast<int>(b2.state));
}

TEST_F(RecycleServiceTest, PurgeTreeTooLarge) {
    // 注入较小上限，避免造 10001 行。
    cm::logic::RecycleService service(*m_pool, *m_cache, 3);

    const std::uint64_t dirId = createChild(0, "big", 0, true);
    ASSERT_GT(dirId, std::uint64_t(0));
    for (int i = 0; i < 4; ++i) {
        ASSERT_GT(createChild(dirId, "c" + std::to_string(i), 0, true),
                  std::uint64_t(0));
    }

    cm::protocol::PurgeRs rs;
    EXPECT_EQ(cm::ErrorCode::TooManyNodes,
              service.purge(m_uid, makePurgeRq(dirId), &rs));
    EXPECT_EQ(3008u, rs.code);

    // 不部分删除：目录仍在。
    cm::db::FileDao dao(*m_conn);
    cm::db::UserFileRow node;
    EXPECT_EQ(cm::ErrorCode::Ok, dao.findUserFile(m_uid, dirId, &node));
}

TEST_F(RecycleServiceTest, PurgeIdempotent) {
    const std::uint64_t fid = createBlob(1024);
    const std::uint64_t ufId = createNode(m_uid, "idem.bin", fid);

    cm::protocol::PurgeRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok, m_service->purge(m_uid, makePurgeRq(ufId), &rs));
    EXPECT_EQ(1u, rs.filesDeleted);
    EXPECT_EQ(1u, rs.blobsOrphaned);  // 活跃文件直接删，补减到 0

    // 第二次：节点已不存在 → code=0，0 计数。
    cm::protocol::PurgeRs again;
    ASSERT_EQ(cm::ErrorCode::Ok, m_service->purge(m_uid, makePurgeRq(ufId), &again));
    EXPECT_EQ(0u, again.code);
    EXPECT_EQ(0u, again.filesDeleted);
    EXPECT_EQ(0u, again.dirsDeleted);
    EXPECT_EQ(0u, again.blobsOrphaned);
}

TEST_F(RecycleServiceTest, RecycleListCursorStable) {
    std::vector<std::uint64_t> ufIds;
    for (int i = 0; i < 8; ++i) {
        const std::uint64_t fid = createBlob(1024);
        const std::uint64_t ufId =
            createNode(m_uid, "r" + std::to_string(i) + ".bin", fid);
        ASSERT_GT(ufId, std::uint64_t(0));
        cm::protocol::DeleteFileRs rs;
        ASSERT_EQ(cm::ErrorCode::Ok,
                  m_service->deleteFile(m_uid, makeDeleteRq(ufId), &rs));
        ufIds.push_back(ufId);
    }

    // keyset 分页，每页 3 条，收集所有，断言无重无漏且顺序稳定（deleted_at/uf_id 降序）。
    std::set<std::uint64_t> seen;
    std::vector<std::uint64_t> collected;
    std::uint64_t cursorMs = 0;
    std::uint64_t cursorId = 0;
    int pages = 0;
    for (;;) {
        cm::protocol::RecycleListRs rs;
        ASSERT_EQ(cm::ErrorCode::Ok,
                  m_service->listRecycle(m_uid, makeListRq(3, cursorMs, cursorId),
                                         &rs));
        ASSERT_EQ(0u, rs.code);
        ++pages;
        ASSERT_LT(pages, 10);
        for (std::size_t i = 0; i < rs.items.size(); ++i) {
            const std::uint64_t id = rs.items[i].ufId;
            collected.push_back(id);
            ASSERT_TRUE(seen.insert(id).second) << "duplicate uf_id " << id;
        }
        if (rs.hasMore == 0) {
            break;
        }
        cursorMs = rs.nextDeletedMs;
        cursorId = rs.nextId;
    }

    ASSERT_EQ(std::size_t(8), collected.size());
    EXPECT_EQ(std::size_t(8), seen.size());
    EXPECT_EQ(3, pages);  // 8 条 / 每页 3 → 3 页

    // 软删顺序 = 创建顺序，回收站按 (deleted_at DESC, uf_id DESC) → 逆序返回。
    std::vector<std::uint64_t> expected(ufIds.rbegin(), ufIds.rend());
    EXPECT_EQ(expected, collected);
}

}  // namespace
