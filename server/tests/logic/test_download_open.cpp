#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "cache/cache_store.h"
#include "cache/redis_pool.h"
#include "dao_fixture.h"
#include "db/file_dao.h"
#include "db/prepared_statement.h"
#include "logic/download_service.h"
#include "logic/upload_fs.h"

namespace {

const std::uint32_t kChunkSize = 4194304;

bool redisEnabled() {
    const char* value = std::getenv("CM_TEST_REDIS");
    return value != NULL && std::string(value) == "1";
}

std::string envOr(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != NULL ? std::string(value) : std::string(fallback);
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

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

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

class DownloadOpenServiceTest : public cm::test::DaoTestBase {
protected:
    void SetUp() override {
        cm::test::DaoTestBase::SetUp();
        if (m_pool == NULL) {
            return;  // 基类已 GTEST_SKIP（缺 MySQL）
        }
        if (!redisEnabled()) {
            GTEST_SKIP() << "set CM_TEST_REDIS=1 (db 15) to run DownloadService tests";
            return;
        }

        char templ[] = "/tmp/cm_download_XXXXXX";
        char* dir = ::mkdtemp(templ);
        ASSERT_TRUE(dir != NULL) << "mkdtemp failed";
        m_root = std::string(dir);

        std::string error;
        m_redis = cm::cache::RedisPool::create(redisOptions(), &error);
        ASSERT_TRUE(m_redis) << error;
        m_cache.reset(
            new cm::cache::CacheStore(*m_redis, cm::cache::CacheStoreOptions()));
        m_service.reset(new cm::logic::DownloadService(*m_pool, *m_cache, m_root));
        createUser();
    }

    void TearDown() override {
        if (m_cache) {
            for (std::size_t i = 0; i < m_tickets.size(); ++i) {
                m_cache->invalidate(
                    cm::cache::CacheKey::downloadTicket(m_tickets[i]));
            }
        }
        m_service.reset();
        m_cache.reset();
        if (m_redis) {
            m_redis->shutdown();
        }
        m_redis.reset();
        if (!m_root.empty()) {
            cm::logic::removeTree(m_root);
            m_root.clear();
        }
        cm::test::DaoTestBase::TearDown();
    }

    cm::protocol::DownloadOpenRq makeRequest(std::uint64_t ufId,
                                             std::uint32_t startBlock = 0) {
        cm::protocol::DownloadOpenRq rq;
        rq.ufId = ufId;
        rq.shareTicket.clear();
        rq.startBlock = startBlock;
        return rq;
    }

    cm::db::BlobRow loadBlob(std::uint64_t fid) {
        cm::db::BlobRow blob;
        cm::db::FileDao dao(*m_conn);
        EXPECT_EQ(cm::ErrorCode::Ok, dao.getBlobById(fid, &blob));
        return blob;
    }

    bool setBlobState(std::uint64_t fid, std::uint8_t state) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "UPDATE t_file SET f_state = ? WHERE f_id = ?");
        if (!stmt || !stmt->bindUInt64(0, state) || !stmt->bindUInt64(1, fid)) {
            return false;
        }
        return stmt->execute();
    }

    void trackTicket(const std::string& ticket) {
        if (!ticket.empty()) {
            m_tickets.push_back(ticket);
        }
    }

    std::string m_root;
    std::shared_ptr<cm::cache::RedisPool> m_redis;
    std::unique_ptr<cm::cache::CacheStore> m_cache;
    std::unique_ptr<cm::logic::DownloadService> m_service;
    std::vector<std::string> m_tickets;
};

TEST_F(DownloadOpenServiceTest, DownloadOpenReturnsTicket) {
    const std::uint64_t uid = m_uid;
    const std::uint64_t fid = createBlob(5000);
    ASSERT_GT(fid, std::uint64_t(0));
    const cm::db::BlobRow blob = loadBlob(fid);
    const std::uint64_t ufId = createNode(uid, "video.mp4", fid);
    ASSERT_GT(ufId, std::uint64_t(0));

    cm::protocol::DownloadOpenRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->openDownload(uid, makeRequest(ufId), &rs));
    trackTicket(rs.ticket);

    EXPECT_EQ(0u, rs.code);
    EXPECT_EQ(fid, rs.fId);
    EXPECT_EQ(std::uint64_t(5000), rs.size);
    EXPECT_EQ(blob.md5, rs.fileMd5);
    EXPECT_EQ(kChunkSize, rs.blockSize);
    EXPECT_EQ(1u, rs.blockCount);
    EXPECT_EQ(0u, rs.startBlock);
    EXPECT_EQ(std::size_t(64), rs.ticket.size());
    for (std::size_t i = 0; i < rs.ticket.size(); ++i) {
        const char c = rs.ticket[i];
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
    EXPECT_GT(rs.ticketExpireMs, nowMs());
    EXPECT_EQ(0, static_cast<int>(rs.hls));

    // ticket 只在 Redis：value = uf_id|f_id|uid|size|md5。
    std::string cached;
    ASSERT_EQ(cm::cache::CacheStore::Result::Hit,
              m_cache->get(cm::cache::CacheKey::downloadTicket(rs.ticket),
                           &cached));
    const std::string expected = std::to_string(ufId) + "|" +
                                 std::to_string(fid) + "|" +
                                 std::to_string(uid) + "|5000|" + blob.md5;
    EXPECT_EQ(expected, cached);
}

TEST_F(DownloadOpenServiceTest, DownloadOpenRejectsDirectory) {
    const std::uint64_t dirId = createNode(m_uid, "dir", 0);
    ASSERT_GT(dirId, std::uint64_t(0));

    cm::protocol::DownloadOpenRs rs;
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              m_service->openDownload(m_uid, makeRequest(dirId), &rs));
    EXPECT_EQ(1001u, rs.code);
    EXPECT_TRUE(rs.ticket.empty());
}

TEST_F(DownloadOpenServiceTest, DownloadOpenRejectsOtherUser) {
    const std::uint64_t owner = m_uid;
    const std::uint64_t fid = createBlob(1024);
    const std::uint64_t ufId = createNode(owner, "private.bin", fid);
    ASSERT_GT(ufId, std::uint64_t(0));

    const std::uint64_t other = createUser();
    ASSERT_NE(owner, other);

    cm::protocol::DownloadOpenRs rs;
    EXPECT_EQ(cm::ErrorCode::NotFound,
              m_service->openDownload(other, makeRequest(ufId), &rs));
    EXPECT_EQ(1002u, rs.code);
    EXPECT_TRUE(rs.ticket.empty());
}

TEST_F(DownloadOpenServiceTest, DownloadOpenRejectsDeletedNode) {
    const std::uint64_t fid = createBlob(1024);
    const std::uint64_t ufId = createNode(m_uid, "gone.bin", fid);
    bool already = false;
    cm::db::FileDao dao(*m_conn);
    ASSERT_EQ(cm::ErrorCode::Ok, dao.softDelete(m_uid, ufId, &already));

    cm::protocol::DownloadOpenRs rs;
    EXPECT_EQ(cm::ErrorCode::NotFound,
              m_service->openDownload(m_uid, makeRequest(ufId), &rs));
    EXPECT_EQ(1002u, rs.code);
    EXPECT_TRUE(rs.ticket.empty());
}

TEST_F(DownloadOpenServiceTest, DownloadOpenRejectsBlobNotReady) {
    const std::uint64_t fid = createBlob(1024);
    ASSERT_TRUE(setBlobState(fid, 2));
    const std::uint64_t ufId = createNode(m_uid, "orphan.bin", fid);

    cm::protocol::DownloadOpenRs rs;
    EXPECT_EQ(cm::ErrorCode::FileGone,
              m_service->openDownload(m_uid, makeRequest(ufId), &rs));
    EXPECT_EQ(3001u, rs.code);
    EXPECT_TRUE(rs.ticket.empty());
}

TEST_F(DownloadOpenServiceTest, DownloadOpenZeroCopyFlagMatchesBuild) {
    const std::uint64_t fid = createBlob(1024);
    const std::uint64_t ufId = createNode(m_uid, "zc.bin", fid);

    cm::protocol::DownloadOpenRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->openDownload(m_uid, makeRequest(ufId), &rs));
    trackTicket(rs.ticket);

#if defined(CM_DOWNLOAD_ZEROCOPY) && (CM_DOWNLOAD_ZEROCOPY)
    EXPECT_EQ(1, static_cast<int>(rs.zeroCopy));
#else
    EXPECT_EQ(0, static_cast<int>(rs.zeroCopy));
#endif
}

TEST_F(DownloadOpenServiceTest, DownloadOpenHlsFlagFromSidecar) {
    const std::uint64_t fid = createBlob(1024);
    const cm::db::BlobRow blob = loadBlob(fid);
    const std::uint64_t ufId = createNode(m_uid, "hls.bin", fid);

    const std::string hlsDir =
        m_root + "/blobs/" + blob.path.substr(0, blob.path.find_last_of('/'));
    ASSERT_TRUE(ensureDir(hlsDir));
    ASSERT_TRUE(writeFile(m_root + "/blobs/" + blob.path + ".m3u8",
                          "#EXTM3U\n"));

    cm::protocol::DownloadOpenRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_service->openDownload(m_uid, makeRequest(ufId), &rs));
    trackTicket(rs.ticket);
    EXPECT_EQ(1, static_cast<int>(rs.hls));
}

}  // namespace
