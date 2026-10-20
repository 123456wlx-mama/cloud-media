#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cache/cache_store.h"
#include "cache/redis_pool.h"
#include "cm/base/buffer.h"
#include "cm/base/thread_pool.h"
#include "cm/protocol/codec.h"
#include "cm/protocol/frame.h"
#include "cm/protocol/message_type.h"
#include "dao_fixture.h"
#include "db/file_dao.h"
#include "logic/block_sender.h"
#include "logic/download_ticket.h"
#include "logic/upload_fs.h"

namespace {

const std::uint32_t kChunkSize = 4194304;

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

std::vector<char> makeData(std::size_t size) {
    std::vector<char> data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>((i * 31U + 7U) & 0xFF);
    }
    return data;
}

// 第 index 块的期望字节（offset = index*4MB，末块可短）。
std::string blockOf(const std::vector<char>& data, std::uint32_t index) {
    const std::size_t offset = static_cast<std::size_t>(index) * kChunkSize;
    const std::size_t len =
        std::min<std::size_t>(kChunkSize, data.size() - offset);
    return std::string(data.data() + offset, len);
}

struct BlockResult {
    cm::ErrorCode code;
    std::string frame;

    BlockResult() : code(cm::ErrorCode::UnknownError) {}
};

class BlockSenderBufferedTest : public cm::test::DaoTestBase {
protected:
    void SetUp() override {
        cm::test::DaoTestBase::SetUp();
        if (m_pool == nullptr) {
            return;  // 基类已 GTEST_SKIP（缺 MySQL）
        }
        if (!redisEnabled()) {
            GTEST_SKIP() << "set CM_TEST_REDIS=1 (db 15) to run BlockSender tests";
            return;
        }

        char templ[] = "/tmp/cm_blocksender_XXXXXX";
        char* dir = ::mkdtemp(templ);
        ASSERT_TRUE(dir != nullptr) << "mkdtemp failed";
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

    std::uint64_t makeBlob(std::uint64_t size) {
        return createBlob(size);
    }

    // 把 blob 的物理文件写到 dataDir/blobs/<f_path>（createBlob 只建 DB 行）。
    void writeBlobFile(std::uint64_t fid, const std::vector<char>& data) {
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

    // 用真实 IO 池构造 sender；返回的 pool 必须晚于 sender 析构。
    std::unique_ptr<cm::logic::BlockSender> makeSender(
        std::unique_ptr<cm::ThreadPool>* pool) {
        pool->reset(new cm::ThreadPool(2, "test-io"));
        cm::ThreadPool* raw = pool->get();
        std::unique_ptr<cm::logic::BlockSender> sender(
            new cm::logic::BlockSender(
                *m_pool, *m_redis,
                [raw](cm::logic::BlockSender::Task t) {
                    return raw->run(std::move(t));
                },
                m_root));
        return sender;
    }

    // 同步等待一次取块完成（done 在 IO 线程触发）。
    BlockResult runOne(cm::logic::BlockSender& sender, std::uint64_t uid,
                       const std::string& ticket, std::uint64_t fid,
                       std::uint32_t blockIndex, std::uint32_t seq) {
        std::mutex mtx;
        std::condition_variable cv;
        BlockResult result;
        bool done = false;
        sender.sendBlock(uid, ticket, fid, blockIndex, seq,
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

TEST_F(BlockSenderBufferedTest, BufferedBlockWhole) {
    const std::vector<char> data = makeData(kChunkSize);  // 恰好 4MB，1 块
    const std::uint64_t fid = makeBlob(kChunkSize);
    writeBlobFile(fid, data);
    const std::string ticket = issueTicket(m_uid, fid);

    std::unique_ptr<cm::ThreadPool> pool;
    std::unique_ptr<cm::logic::BlockSender> sender = makeSender(&pool);

    const BlockResult result = runOne(*sender, m_uid, ticket, fid, 0, 42);
    ASSERT_EQ(cm::ErrorCode::Ok, result.code);
    ASSERT_EQ(std::size_t(cm::protocol::kHeaderSize + kChunkSize),
              result.frame.size());

    // 解码并校验：CRC 正确（decode 返回 Ok 即通过）、type/seq/payload 一致。
    cm::Buffer buffer;
    ASSERT_TRUE(buffer.append(result.frame));
    cm::protocol::Frame decoded;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::NeedMore;
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::protocol::Codec::decode(buffer, &decoded, &status));
    EXPECT_EQ(cm::protocol::DecodeStatus::Ok, status);
    EXPECT_EQ(static_cast<std::uint16_t>(
                  cm::protocol::MessageType::DownloadDataRs),
              decoded.type);
    EXPECT_EQ(42u, decoded.seq);
    EXPECT_EQ(blockOf(data, 0), decoded.payload);
}

TEST_F(BlockSenderBufferedTest, BufferedBlockLastShort) {
    const std::vector<char> data = makeData(kChunkSize + 137);  // 2 块，末块短
    const std::uint64_t fid = makeBlob(data.size());
    writeBlobFile(fid, data);
    const std::string ticket = issueTicket(m_uid, fid);

    std::unique_ptr<cm::ThreadPool> pool;
    std::unique_ptr<cm::logic::BlockSender> sender = makeSender(&pool);

    const BlockResult result = runOne(*sender, m_uid, ticket, fid, 1, 7);
    ASSERT_EQ(cm::ErrorCode::Ok, result.code);
    ASSERT_EQ(std::size_t(cm::protocol::kHeaderSize + 137), result.frame.size());

    cm::Buffer buffer;
    ASSERT_TRUE(buffer.append(result.frame));
    cm::protocol::Frame decoded;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::NeedMore;
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::protocol::Codec::decode(buffer, &decoded, &status));
    EXPECT_EQ(cm::protocol::DecodeStatus::Ok, status);
    EXPECT_EQ(7u, decoded.seq);
    EXPECT_EQ(blockOf(data, 1), decoded.payload);
}

TEST_F(BlockSenderBufferedTest, BufferedBlockOutOfRange) {
    const std::uint64_t fid = makeBlob(kChunkSize);  // 1 块，index 0 唯一合法
    const std::string ticket = issueTicket(m_uid, fid);

    std::unique_ptr<cm::ThreadPool> pool;
    std::unique_ptr<cm::logic::BlockSender> sender = makeSender(&pool);

    const BlockResult result = runOne(*sender, m_uid, ticket, fid, 1, 0);
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, result.code);
    EXPECT_TRUE(result.frame.empty());
}

TEST_F(BlockSenderBufferedTest, BufferedBlockReadFailure) {
    // DB 有 blob 行但磁盘无文件：readBlock 的 open 失败 → 3001。
    const std::uint64_t fid = makeBlob(kChunkSize);
    const std::string ticket = issueTicket(m_uid, fid);

    std::unique_ptr<cm::ThreadPool> pool;
    std::unique_ptr<cm::logic::BlockSender> sender = makeSender(&pool);

    const BlockResult result = runOne(*sender, m_uid, ticket, fid, 0, 0);
    EXPECT_EQ(cm::ErrorCode::FileGone, result.code);
    EXPECT_TRUE(result.frame.empty());
}

TEST_F(BlockSenderBufferedTest, BufferedBlockConcurrencyCap) {
    const std::vector<char> data = makeData(kChunkSize);
    const std::uint64_t fid = makeBlob(kChunkSize);
    writeBlobFile(fid, data);
    const std::string ticket = issueTicket(m_uid, fid);

    // 手动执行器：任务收进列表，由测试决定何时执行，保证并发上限断言确定。
    std::vector<cm::logic::BlockSender::Task> tasks;
    std::mutex taskMutex;
    cm::logic::BlockSender::Executor executor =
        [&tasks, &taskMutex](cm::logic::BlockSender::Task t) {
            std::lock_guard<std::mutex> lock(taskMutex);
            tasks.push_back(std::move(t));
            return true;
        };
    cm::logic::BlockSender sender(*m_pool, *m_redis, executor, m_root);

    const int kTotal = 17;  // 上限 16，第 17 个必须排队
    std::vector<cm::ErrorCode> codes(kTotal, cm::ErrorCode::UnknownError);
    std::vector<std::string> frames(kTotal);
    std::atomic<int> completed(0);

    for (int i = 0; i < kTotal; ++i) {
        sender.sendBlock(m_uid, ticket, fid, 0, static_cast<std::uint32_t>(i),
                         [&, i](cm::ErrorCode code, std::string frame) {
                             codes[i] = code;
                             frames[i] = std::move(frame);
                             completed.fetch_add(1);
                         });
    }

    // 前 16 个进 IO，第 17 个排队，不 OOM。
    EXPECT_EQ(std::size_t(16), sender.inFlight());
    EXPECT_EQ(std::size_t(1), sender.pending());
    {
        std::lock_guard<std::mutex> lock(taskMutex);
        EXPECT_EQ(std::size_t(16), tasks.size());
    }

    // 逐个执行，直到队列清空（每个完成后自动 dequeue 下一个）。
    for (;;) {
        cm::logic::BlockSender::Task t;
        {
            std::lock_guard<std::mutex> lock(taskMutex);
            if (tasks.empty()) {
                break;
            }
            t = std::move(tasks.front());
            tasks.erase(tasks.begin());
        }
        t();
    }

    EXPECT_EQ(std::size_t(0), sender.inFlight());
    EXPECT_EQ(std::size_t(0), sender.pending());
    EXPECT_EQ(kTotal, completed.load());
    for (int i = 0; i < kTotal; ++i) {
        ASSERT_EQ(cm::ErrorCode::Ok, codes[i]);
        EXPECT_EQ(std::size_t(cm::protocol::kHeaderSize + kChunkSize),
                  frames[i].size());
    }
}

}  // namespace
