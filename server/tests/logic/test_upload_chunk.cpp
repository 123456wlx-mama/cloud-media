#include <gtest/gtest.h>

#include <dirent.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "cm/base/md5.h"
#include "dao_fixture.h"
#include "db/prepared_statement.h"
#include "db/transfer_dao.h"
#include "logic/upload_chunk_handler.h"

namespace {

const std::uint32_t kChunkSize = 4194304;

std::string makeData(std::size_t size, char fill) {
    return std::string(size, fill);
}

void removeTree(const std::string& path) {
    DIR* dir = ::opendir(path.c_str());
    if (dir != nullptr) {
        struct dirent* entry = nullptr;
        while ((entry = ::readdir(dir)) != nullptr) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") {
                continue;
            }
            removeTree(path + "/" + name);
        }
        ::closedir(dir);
    }
    std::remove(path.c_str());
}

std::string readFile(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        return std::string();
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

class UploadChunkServiceTest : public cm::test::DaoTestBase {
protected:
    void SetUp() override {
        cm::test::DaoTestBase::SetUp();
        if (m_pool == nullptr) {
            return;  // 基类已 GTEST_SKIP（缺 MySQL）
        }

        char templ[] = "/tmp/cm_chunk_XXXXXX";
        char* dir = ::mkdtemp(templ);
        ASSERT_TRUE(dir != nullptr) << "mkdtemp failed";
        m_root = std::string(dir);
        m_handler.reset(new cm::logic::UploadChunkHandler(*m_pool, m_root));
        createUser();
    }

    void TearDown() override {
        m_handler.reset();
        if (!m_root.empty()) {
            removeTree(m_root);
            m_root.clear();
        }
        cm::test::DaoTestBase::TearDown();
    }

    cm::protocol::UploadChunkRq makeChunk(const std::string& taskId,
                                          std::uint32_t index,
                                          std::uint64_t offset,
                                          const std::string& data) {
        cm::protocol::UploadChunkRq rq;
        rq.taskId = taskId;
        rq.chunkIndex = index;
        rq.offset = offset;
        rq.chunkMd5 = cm::md5Hex(data);
        rq.data = data;
        return rq;
    }

    std::string chunkPath(const std::string& taskId, std::uint32_t index) {
        char name[32];
        std::snprintf(name, sizeof(name), "%u.bin", index);
        return m_root + "/tmp/" + taskId + "/" + name;
    }

    bool fileExists(const std::string& path) {
        return ::access(path.c_str(), F_OK) == 0;
    }

    bool findChunkRow(const std::string& taskId,
                      std::uint32_t index,
                      cm::db::ChunkRow* out) {
        cm::db::TransferDao dao(*m_conn);
        return dao.findChunk(taskId, index, out) == cm::ErrorCode::Ok;
    }

    bool expireTask(const std::string& taskId) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "UPDATE t_transfer_task "
            "SET tr_expire_at = DATE_SUB(NOW(6), INTERVAL 1 HOUR) "
            "WHERE tr_task_id = ?");
        if (!stmt || !stmt->bindString(0, taskId)) {
            return false;
        }
        return stmt->execute();
    }

    std::string m_root;
    std::unique_ptr<cm::logic::UploadChunkHandler> m_handler;
};

TEST_F(UploadChunkServiceTest, UploadChunkNewCompletes) {
    const std::string taskId = createTask(m_uid, 13, 50ULL * 1024 * 1024);
    ASSERT_FALSE(taskId.empty());
    const std::string data = makeData(kChunkSize, 'a');

    cm::protocol::UploadChunkRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_handler->handle(m_uid, 1, makeChunk(taskId, 0, 0, data), nullptr, &rs));

    EXPECT_EQ(0u, rs.code);
    EXPECT_EQ(0u, rs.chunkIndex);
    EXPECT_EQ(0, static_cast<int>(rs.duplicate));
    EXPECT_EQ(1u, rs.doneChunks);
    EXPECT_EQ(std::uint64_t(kChunkSize), rs.doneBytes);
    EXPECT_EQ(1u, rs.nextHint);

    EXPECT_TRUE(fileExists(chunkPath(taskId, 0)));
    EXPECT_EQ(data, readFile(chunkPath(taskId, 0)));

    cm::db::ChunkRow row;
    ASSERT_TRUE(findChunkRow(taskId, 0, &row));
    EXPECT_EQ(1, static_cast<int>(row.state));
    EXPECT_EQ(std::uint64_t(0), row.offset);
    EXPECT_EQ(kChunkSize, row.size);
    EXPECT_EQ(cm::md5Hex(data), row.md5);
    EXPECT_EQ(std::string("tmp/") + taskId + "/0.bin", row.path);
}

TEST_F(UploadChunkServiceTest, UploadChunkDuplicateNoCount) {
    const std::string taskId = createTask(m_uid, 13, 50ULL * 1024 * 1024);
    const std::string data = makeData(kChunkSize, 'a');
    const cm::protocol::UploadChunkRq rq = makeChunk(taskId, 0, 0, data);

    cm::protocol::UploadChunkRs first;
    ASSERT_EQ(cm::ErrorCode::Ok, m_handler->handle(m_uid, 1, rq, nullptr, &first));
    ASSERT_EQ(1u, first.doneChunks);

    cm::protocol::UploadChunkRs second;
    ASSERT_EQ(cm::ErrorCode::Ok, m_handler->handle(m_uid, 2, rq, nullptr, &second));
    EXPECT_EQ(0u, second.code);
    EXPECT_EQ(1, static_cast<int>(second.duplicate));
    EXPECT_EQ(1u, second.doneChunks);
    EXPECT_EQ(std::uint64_t(kChunkSize), second.doneBytes);
}

TEST_F(UploadChunkServiceTest, UploadChunkDuplicateDifferentMd5Rejected) {
    const std::string taskId = createTask(m_uid, 13, 50ULL * 1024 * 1024);
    const std::string good = makeData(kChunkSize, 'a');
    const std::string evil = makeData(kChunkSize, 'b');

    cm::protocol::UploadChunkRs first;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_handler->handle(m_uid, 1, makeChunk(taskId, 0, 0, good), nullptr, &first));

    cm::protocol::UploadChunkRs second;
    EXPECT_EQ(cm::ErrorCode::Md5Mismatch,
              m_handler->handle(m_uid, 2, makeChunk(taskId, 0, 0, evil), nullptr, &second));
    EXPECT_EQ(3003u, second.code);

    // 已确认的分片不能被覆盖：文件内容与 DB 里的 MD5 都还是第一份。
    EXPECT_EQ(good, readFile(chunkPath(taskId, 0)));
    cm::db::ChunkRow row;
    ASSERT_TRUE(findChunkRow(taskId, 0, &row));
    EXPECT_EQ(cm::md5Hex(good), row.md5);
    EXPECT_EQ(1u, first.doneChunks);
}

TEST_F(UploadChunkServiceTest, UploadChunkOutOfOrder) {
    const std::string taskId = createTask(m_uid, 13, 50ULL * 1024 * 1024);
    const std::uint32_t order[3] = {2, 0, 1};

    std::uint32_t expectedDone = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        const std::uint32_t index = order[i];
        const std::string data = makeData(kChunkSize, static_cast<char>('a' + index));
        cm::protocol::UploadChunkRs rs;
        ASSERT_EQ(cm::ErrorCode::Ok,
                  m_handler->handle(m_uid, static_cast<std::uint32_t>(i + 1),
                                    makeChunk(taskId, index,
                                              static_cast<std::uint64_t>(index) * kChunkSize,
                                              data),
                                    nullptr, &rs));
        ++expectedDone;
        EXPECT_EQ(expectedDone, rs.doneChunks);
        EXPECT_EQ(static_cast<std::uint64_t>(expectedDone) * kChunkSize, rs.doneBytes);
    }
}

TEST_F(UploadChunkServiceTest, UploadChunkOffsetMismatch) {
    const std::string taskId = createTask(m_uid, 13, 50ULL * 1024 * 1024);
    const std::string data = makeData(kChunkSize, 'a');

    cm::protocol::UploadChunkRs rs;
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              m_handler->handle(m_uid, 1,
                                makeChunk(taskId, 1, 0, data),  // offset 应为 4MB
                                nullptr, &rs));
    EXPECT_EQ(1001u, rs.code);

    cm::db::ChunkRow row;
    EXPECT_FALSE(findChunkRow(taskId, 1, &row));
    EXPECT_FALSE(fileExists(chunkPath(taskId, 1)));
}

TEST_F(UploadChunkServiceTest, UploadChunkWrongSizeLast) {
    const std::uint64_t totalSize = kChunkSize + 100;
    const std::string taskId = createTask(m_uid, 2, totalSize);

    cm::protocol::UploadChunkRs wrong;
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              m_handler->handle(m_uid, 1,
                                makeChunk(taskId, 1, kChunkSize, makeData(200, 'x')),
                                nullptr, &wrong));
    EXPECT_EQ(1001u, wrong.code);

    cm::protocol::UploadChunkRs ok;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_handler->handle(m_uid, 2,
                                makeChunk(taskId, 1, kChunkSize, makeData(100, 'x')),
                                nullptr, &ok));
    EXPECT_EQ(1u, ok.doneChunks);
    EXPECT_EQ(std::uint64_t(100), ok.doneBytes);

    cm::db::ChunkRow row;
    ASSERT_TRUE(findChunkRow(taskId, 1, &row));
    EXPECT_EQ(100u, row.size);
}

TEST_F(UploadChunkServiceTest, UploadChunkExpiredTask) {
    const std::string taskId = createTask(m_uid, 13, 50ULL * 1024 * 1024);
    ASSERT_TRUE(expireTask(taskId));
    const std::string data = makeData(kChunkSize, 'a');

    cm::protocol::UploadChunkRs rs;
    EXPECT_EQ(cm::ErrorCode::TaskExpired,
              m_handler->handle(m_uid, 1, makeChunk(taskId, 0, 0, data), nullptr, &rs));
    EXPECT_EQ(3007u, rs.code);

    cm::db::ChunkRow row;
    EXPECT_FALSE(findChunkRow(taskId, 0, &row));
    EXPECT_FALSE(fileExists(chunkPath(taskId, 0)));
}

TEST_F(UploadChunkServiceTest, UploadChunkTamperedPayloadDeferredToMerge) {
    // O7-a 之后收片阶段不再重算分片 MD5：传输完整性由帧 CRC（Codec::verifyFrameCrc）
    // 负责，内容完整性由 complete 阶段 mergeChunks 逐片重算兜底（坏片进 badIndexes，
    // 见 test_upload_complete 的 badIndexes 用例）。因此这里断言"收下并落盘"。
    const std::string taskId = createTask(m_uid, 13, 50ULL * 1024 * 1024);
    cm::protocol::UploadChunkRq rq = makeChunk(taskId, 0, 0, makeData(kChunkSize, 'c'));
    rq.data[100] = 'd';  // 篡改一个字节，chunk_md5 不再匹配

    cm::protocol::UploadChunkRs rs;
    EXPECT_EQ(cm::ErrorCode::Ok, m_handler->handle(m_uid, 1, rq, nullptr, &rs));
    EXPECT_EQ(0u, rs.code);

    cm::db::ChunkRow row;
    EXPECT_TRUE(findChunkRow(taskId, 0, &row));
}

TEST_F(UploadChunkServiceTest, UploadChunkInFlightLimit) {
    int pauses = 0;
    int resumes = 0;
    cm::logic::UploadInFlightGate gate(
        2, [&pauses]() { ++pauses; }, [&resumes]() { ++resumes; });

    EXPECT_TRUE(gate.tryBegin());
    EXPECT_TRUE(gate.tryBegin());
    EXPECT_EQ(std::size_t(2), gate.inFlight());

    // 第 3 个在途分片：拒绝并 pauseRead。
    EXPECT_FALSE(gate.tryBegin());
    EXPECT_EQ(1, pauses);
    EXPECT_EQ(std::size_t(2), gate.inFlight());

    EXPECT_TRUE(gate.end());  // 2 → 1，恢复读
    EXPECT_EQ(1, resumes);
    EXPECT_EQ(std::size_t(1), gate.inFlight());

    EXPECT_FALSE(gate.end());  // 1 → 0，已经恢复过，不再回调
    EXPECT_EQ(1, resumes);
    EXPECT_EQ(std::size_t(0), gate.inFlight());

    // handler 结束必须释放名额：塞满 2 个后跑一片，计数回到 1。
    const std::string taskId = createTask(m_uid, 13, 50ULL * 1024 * 1024);
    cm::logic::UploadInFlightGate busy(
        2, [&pauses]() { ++pauses; }, [&resumes]() { ++resumes; });
    ASSERT_TRUE(busy.tryBegin());
    ASSERT_TRUE(busy.tryBegin());

    cm::protocol::UploadChunkRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_handler->handle(m_uid, 1,
                                makeChunk(taskId, 0, 0, makeData(kChunkSize, 'a')),
                                &busy, &rs));
    EXPECT_EQ(std::size_t(1), busy.inFlight());
}

}  // namespace
