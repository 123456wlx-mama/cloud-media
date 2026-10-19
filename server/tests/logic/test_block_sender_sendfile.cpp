#include <gtest/gtest.h>

#include "logic/block_sender_factory.h"

// 工厂测试：ON/OFF 两种构建都运行。
TEST(BlockSenderFactoryTest, BlockSenderFactorySelectsBufferedWhenDisabled) {
#if defined(CM_DOWNLOAD_ZEROCOPY) && (CM_DOWNLOAD_ZEROCOPY)
    EXPECT_EQ(cm::logic::BlockSenderKind::Sendfile,
              cm::logic::BlockSenderFactory::select());
#else
    EXPECT_EQ(cm::logic::BlockSenderKind::Buffered,
              cm::logic::BlockSenderFactory::select());
#endif
}

#if defined(CM_DOWNLOAD_ZEROCOPY) && (CM_DOWNLOAD_ZEROCOPY)

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cache/cache_store.h"
#include "cache/redis_pool.h"
#include "cm/base/byte_order.h"
#include "cm/base/crc32.h"
#include "cm/base/thread_pool.h"
#include "cm/protocol/frame.h"
#include "cm/protocol/message_type.h"
#include "dao_fixture.h"
#include "db/file_dao.h"
#include "logic/block_crc.h"
#include "logic/block_sender.h"
#include "logic/download_ticket.h"
#include "logic/sendfile_block_sender.h"
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

std::vector<char> makeData(std::size_t size) {
    std::vector<char> data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>((i * 31U + 7U) & 0xFF);
    }
    return data;
}

std::string blockOf(const std::vector<char>& data, std::uint32_t index) {
    const std::size_t offset = static_cast<std::size_t>(index) * kChunkSize;
    const std::size_t len =
        std::min<std::size_t>(kChunkSize, data.size() - offset);
    return std::string(data.data() + offset, len);
}

std::uint32_t directBlockCrc(const std::vector<char>& data,
                             std::uint32_t index) {
    const std::size_t offset = static_cast<std::size_t>(index) * kChunkSize;
    const std::size_t len =
        std::min<std::size_t>(kChunkSize, data.size() - offset);
    cm::Crc32 crc;
    crc.update(data.data() + offset, len);
    return crc.value();
}

// 从 fd 的 offset 处读 len 字节（EINTR/短读循环）。
std::string readFdAt(int fd, std::int64_t offset, std::size_t len) {
    std::string out(len, '\0');
    std::size_t total = 0;
    while (total < len) {
        const ssize_t n = ::pread(
            fd, &out[total], len - total,
            static_cast<off_t>(offset) + static_cast<off_t>(total));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        total += static_cast<std::size_t>(n);
    }
    out.resize(total);
    return out;
}

struct BlockResult {
    cm::ErrorCode code;
    std::string frame;

    BlockResult() : code(cm::ErrorCode::UnknownError) {}
};

class SendfileBlockSenderTest : public cm::test::DaoTestBase {
protected:
    void SetUp() override {
        cm::test::DaoTestBase::SetUp();
        if (m_pool == NULL) {
            return;  // 基类已 GTEST_SKIP（缺 MySQL）
        }
        if (!redisEnabled()) {
            GTEST_SKIP() << "set CM_TEST_REDIS=1 (db 15) to run SendfileBlockSender tests";
            return;
        }

        char templ[] = "/tmp/cm_sendfile_XXXXXX";
        char* dir = ::mkdtemp(templ);
        ASSERT_TRUE(dir != NULL) << "mkdtemp failed";
        m_root = std::string(dir);

        std::string error;
        m_redis = cm::cache::RedisPool::create(redisOptions(), &error);
        ASSERT_TRUE(m_redis) << error;
        m_ticket.reset(new cm::logic::DownloadTicket(*m_redis));
        createUser();
    }

    void TearDown() override {
        m_ticket.reset();
        if (m_redis) {
            // 先归还连接再 shutdown，否则每个用例硬等 5 秒。
            {
                std::unique_ptr<cm::cache::RedisConn> conn = m_redis->acquire();
                if (conn) {
                    for (std::size_t i = 0; i < m_tokens.size(); ++i) {
                        conn->del(
                            cm::cache::CacheKey::downloadTicket(m_tokens[i]));
                    }
                }
            }
            m_redis->shutdown();
        }
        m_redis.reset();
        if (!m_root.empty()) {
            cm::logic::removeTree(m_root);
            m_root.clear();
        }
        cm::test::DaoTestBase::TearDown();
    }

    // 写物理 blob + 生成 .crc sidecar（createBlob 只建 DB 行）。
    void writeBlobWithSidecar(std::uint64_t fid,
                              const std::vector<char>& data) {
        cm::db::BlobRow blob;
        cm::db::FileDao dao(*m_conn);
        ASSERT_EQ(cm::ErrorCode::Ok, dao.getBlobById(fid, &blob));
        const std::string full = m_root + "/blobs/" + blob.path;
        const std::string dir = full.substr(0, full.find_last_of('/'));
        ASSERT_TRUE(cm::logic::makeDirs(dir));
        std::ofstream out(full.c_str(), std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        ASSERT_TRUE(out.good());
        ASSERT_EQ(cm::ErrorCode::Ok,
                  cm::logic::writeBlockCrc(full, data.size()));
    }

    std::string issueTicket(std::uint64_t uid, std::uint64_t fid) {
        std::string token;
        std::uint64_t expireAtMs = 0;
        EXPECT_EQ(cm::ErrorCode::Ok,
                  m_ticket->issue(uid, fid, &token, &expireAtMs));
        EXPECT_EQ(std::size_t(64), token.size());
        m_tokens.push_back(token);
        return token;
    }

    // D-3 缓冲路径：同步等待一帧。
    BlockResult runBuffered(cm::logic::BlockSender& buffered,
                            std::uint64_t uid, const std::string& ticket,
                            std::uint64_t fid, std::uint32_t blockIndex,
                            std::uint32_t seq) {
        std::mutex mtx;
        std::condition_variable cv;
        BlockResult result;
        bool done = false;
        buffered.sendBlock(uid, ticket, fid, blockIndex, seq,
                           [&](cm::ErrorCode code, std::string frame) {
                               std::lock_guard<std::mutex> lock(mtx);
                               result.code = code;
                               result.frame = std::move(frame);
                               done = true;
                               cv.notify_one();
                           });
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&]() { return done; });
        return result;
    }

    std::string m_root;
    std::shared_ptr<cm::cache::RedisPool> m_redis;
    std::unique_ptr<cm::logic::DownloadTicket> m_ticket;
    std::vector<std::string> m_tokens;
};

TEST_F(SendfileBlockSenderTest, SendfileBlockMatchesBuffered) {
    const std::vector<char> data = makeData(kChunkSize);  // 恰好 4MB，1 块
    const std::uint64_t fid = createBlob(kChunkSize);
    writeBlobWithSidecar(fid, data);
    const std::string ticket = issueTicket(m_uid, fid);
    const std::uint32_t seq = 42;

    cm::ThreadPool ioPool(2, "test-io");

    // 缓冲路径（D-3）。
    cm::logic::BlockSender buffered(
        *m_pool, *m_redis,
        [&ioPool](cm::logic::BlockSender::Task t) {
            return ioPool.run(std::move(t));
        },
        m_root);
    const BlockResult bufferedResult = runBuffered(buffered, m_uid, ticket, fid, 0, seq);
    ASSERT_EQ(cm::ErrorCode::Ok, bufferedResult.code);

    // sendfile 路径（D-4）。
    std::string capturedHeader;
    int capturedFd = -1;
    std::int64_t capturedOffset = -1;
    std::size_t capturedLen = 0;
    auto deliver = [&](const std::string& header, int fd, std::int64_t offset,
                       std::size_t len) {
        capturedHeader = header;
        capturedFd = fd;
        capturedOffset = offset;
        capturedLen = len;
        return true;  // 接管 fd
    };
    cm::logic::SendfileBlockSender sender(
        *m_pool, *m_redis,
        [&ioPool](cm::logic::SendfileBlockSender::Task t) {
            return ioPool.run(std::move(t));
        },
        m_root);

    cm::ErrorCode sfResult = cm::ErrorCode::UnknownError;
    sender.sendBlock(m_uid, ticket, fid, 0, seq,
                     deliver, cm::logic::SendfileBlockSender::Fallback(),
                     [&](cm::ErrorCode code) { sfResult = code; });
    // 已知 sidecar：同步交付。
    ASSERT_EQ(cm::ErrorCode::Ok, sfResult);

    // 帧头 19B 与缓冲路径逐字节一致。
    ASSERT_EQ(std::size_t(cm::protocol::kHeaderSize), capturedHeader.size());
    EXPECT_EQ(bufferedResult.frame.substr(0, cm::protocol::kHeaderSize),
              capturedHeader);

    // payload 与缓冲路径逐字节一致。
    ASSERT_GE(capturedFd, 0);
    ASSERT_EQ(std::size_t(kChunkSize), capturedLen);
    EXPECT_EQ(std::int64_t(0), capturedOffset);
    const std::string payload = readFdAt(capturedFd, capturedOffset, capturedLen);
    EXPECT_EQ(bufferedResult.frame.substr(cm::protocol::kHeaderSize), payload);
    EXPECT_EQ(blockOf(data, 0), payload);
    ::close(capturedFd);
}

TEST_F(SendfileBlockSenderTest, SendfileBlockLastShort) {
    const std::vector<char> data = makeData(kChunkSize + 137);  // 2 块，末块短
    const std::uint64_t fid = createBlob(data.size());
    writeBlobWithSidecar(fid, data);
    const std::string ticket = issueTicket(m_uid, fid);

    cm::ThreadPool ioPool(2, "test-io");
    std::string capturedHeader;
    int capturedFd = -1;
    std::int64_t capturedOffset = -1;
    std::size_t capturedLen = 0;
    auto deliver = [&](const std::string& header, int fd, std::int64_t offset,
                       std::size_t len) {
        capturedHeader = header;
        capturedFd = fd;
        capturedOffset = offset;
        capturedLen = len;
        return true;
    };
    cm::logic::SendfileBlockSender sender(
        *m_pool, *m_redis,
        [&ioPool](cm::logic::SendfileBlockSender::Task t) {
            return ioPool.run(std::move(t));
        },
        m_root);

    cm::ErrorCode sfResult = cm::ErrorCode::UnknownError;
    sender.sendBlock(m_uid, ticket, fid, 1, 7,
                     deliver, cm::logic::SendfileBlockSender::Fallback(),
                     [&](cm::ErrorCode code) { sfResult = code; });
    ASSERT_EQ(cm::ErrorCode::Ok, sfResult);

    ASSERT_EQ(std::size_t(137), capturedLen);
    EXPECT_EQ(std::int64_t(kChunkSize), capturedOffset);
    // 帧头 LEN 字段 = 137。
    ASSERT_EQ(std::size_t(cm::protocol::kHeaderSize), capturedHeader.size());
    const std::uint8_t* h =
        reinterpret_cast<const std::uint8_t*>(capturedHeader.data());
    EXPECT_EQ(static_cast<std::uint32_t>(137), cm::readU32BE(h + 7));

    ASSERT_GE(capturedFd, 0);
    const std::string payload = readFdAt(capturedFd, capturedOffset, capturedLen);
    EXPECT_EQ(blockOf(data, 1), payload);
    ::close(capturedFd);
}

TEST_F(SendfileBlockSenderTest, SendfileMissingSidecarFallsBack) {
    // DB 有 blob 行，但磁盘无文件、无 sidecar：懒填充的读块失败 → 回退缓冲。
    const std::uint64_t fid = createBlob(kChunkSize);
    const std::string ticket = issueTicket(m_uid, fid);

    cm::ThreadPool ioPool(2, "test-io");
    std::mutex mtx;
    std::condition_variable cv;
    bool fallbackCalled = false;
    std::uint64_t fbUid = 0;
    std::string fbTicket;
    std::uint64_t fbFid = 0;
    std::uint32_t fbIndex = 999;
    std::uint32_t fbSeq = 0;
    auto fallback = [&](std::uint64_t uid, const std::string& t,
                        std::uint64_t fid2, std::uint32_t blockIndex,
                        std::uint32_t seq) {
        std::lock_guard<std::mutex> lock(mtx);
        fallbackCalled = true;
        fbUid = uid;
        fbTicket = t;
        fbFid = fid2;
        fbIndex = blockIndex;
        fbSeq = seq;
        cv.notify_one();
    };
    cm::logic::SendfileBlockSender sender(
        *m_pool, *m_redis,
        [&ioPool](cm::logic::SendfileBlockSender::Task t) {
            return ioPool.run(std::move(t));
        },
        m_root);

    cm::ErrorCode sfResult = cm::ErrorCode::UnknownError;
    sender.sendBlock(m_uid, ticket, fid, 0, 3,
                     cm::logic::SendfileBlockSender::Deliver(), fallback,
                     [&](cm::ErrorCode code) { sfResult = code; });

    // fallback 在 IO 池线程异步调用。
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [&]() { return fallbackCalled; });

    EXPECT_EQ(m_uid, fbUid);
    EXPECT_EQ(ticket, fbTicket);
    EXPECT_EQ(fid, fbFid);
    EXPECT_EQ(0u, fbIndex);
    EXPECT_EQ(3u, fbSeq);
    EXPECT_EQ(cm::ErrorCode::UnknownError, sfResult);  // done 未被调用
}

TEST_F(SendfileBlockSenderTest, SendfileHeaderBeforePayload) {
    const std::vector<char> data = makeData(kChunkSize);
    const std::uint64_t fid = createBlob(kChunkSize);
    writeBlobWithSidecar(fid, data);
    const std::string ticket = issueTicket(m_uid, fid);
    const std::uint32_t seq = 9;

    cm::ThreadPool ioPool(2, "test-io");
    std::string capturedHeader;
    int capturedFd = -1;
    std::int64_t capturedOffset = -1;
    std::size_t capturedLen = 0;
    auto deliver = [&](const std::string& header, int fd, std::int64_t offset,
                       std::size_t len) {
        capturedHeader = header;
        capturedFd = fd;
        capturedOffset = offset;
        capturedLen = len;
        return true;
    };
    cm::logic::SendfileBlockSender sender(
        *m_pool, *m_redis,
        [&ioPool](cm::logic::SendfileBlockSender::Task t) {
            return ioPool.run(std::move(t));
        },
        m_root);

    cm::ErrorCode sfResult = cm::ErrorCode::UnknownError;
    sender.sendBlock(m_uid, ticket, fid, 0, seq,
                     deliver, cm::logic::SendfileBlockSender::Fallback(),
                     [&](cm::ErrorCode code) { sfResult = code; });
    ASSERT_EQ(cm::ErrorCode::Ok, sfResult);

    // 帧头 19B：magic/version/type/len/seq/crc 全部正确。
    ASSERT_EQ(std::size_t(cm::protocol::kHeaderSize), capturedHeader.size());
    const std::uint8_t* h =
        reinterpret_cast<const std::uint8_t*>(capturedHeader.data());
    EXPECT_EQ(cm::protocol::kMagic, cm::readU32BE(h));
    EXPECT_EQ(static_cast<std::uint8_t>(cm::protocol::kVersion), h[4]);
    EXPECT_EQ(static_cast<std::uint16_t>(
                  cm::protocol::MessageType::DownloadDataRs),
              cm::readU16BE(h + 5));
    EXPECT_EQ(kChunkSize, cm::readU32BE(h + 7));
    EXPECT_EQ(seq, cm::readU32BE(h + 11));

    // CRC = crc32Combine(crc32(header11), blockCrc, len)。
    const std::uint32_t header11Crc = cm::crc32(h + 4, 11);
    const std::uint32_t expectedCrc =
        cm::crc32Combine(header11Crc, directBlockCrc(data, 0), kChunkSize);
    EXPECT_EQ(expectedCrc, cm::readU32BE(h + 15));

    // payload 区由 fd + offset + len 描述，header 先于 payload 交给连接层。
    EXPECT_EQ(std::int64_t(0), capturedOffset);
    EXPECT_EQ(std::size_t(kChunkSize), capturedLen);
    EXPECT_EQ(blockOf(data, 0), readFdAt(capturedFd, capturedOffset, capturedLen));
    ::close(capturedFd);
}

TEST_F(SendfileBlockSenderTest, SendfileFdClosed) {
    const std::vector<char> data = makeData(kChunkSize);
    const std::uint64_t fid = createBlob(kChunkSize);
    writeBlobWithSidecar(fid, data);
    const std::string ticket = issueTicket(m_uid, fid);

    cm::ThreadPool ioPool(2, "test-io");
    int capturedFd = -1;
    auto deliver = [&](const std::string&, int fd, std::int64_t, std::size_t) {
        capturedFd = fd;
        return false;  // 拒绝受理：本类应关闭 fd，不泄漏
    };
    cm::logic::SendfileBlockSender sender(
        *m_pool, *m_redis,
        [&ioPool](cm::logic::SendfileBlockSender::Task t) {
            return ioPool.run(std::move(t));
        },
        m_root);

    cm::ErrorCode sfResult = cm::ErrorCode::UnknownError;
    sender.sendBlock(m_uid, ticket, fid, 0, 1,
                     deliver, cm::logic::SendfileBlockSender::Fallback(),
                     [&](cm::ErrorCode code) { sfResult = code; });
    ASSERT_EQ(cm::ErrorCode::NetworkError, sfResult);
    ASSERT_GE(capturedFd, 0);

    errno = 0;
    EXPECT_EQ(-1, ::fcntl(capturedFd, F_GETFD));
    EXPECT_EQ(EBADF, errno);
}

}  // namespace

#endif  // CM_DOWNLOAD_ZEROCOPY
