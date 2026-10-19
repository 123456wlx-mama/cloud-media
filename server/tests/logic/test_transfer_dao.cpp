#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "dao_fixture.h"
#include "db/prepared_statement.h"
#include "db/transfer_dao.h"

namespace {

const std::uint32_t kChunkSize = 4194304;

class TransferDaoTest : public cm::test::DaoTestBase {
protected:
    void SetUp() override {
        cm::test::DaoTestBase::SetUp();
        if (m_pool == NULL) {
            return;  // 基类已 GTEST_SKIP
        }
        m_dao.reset(new cm::db::TransferDao(*m_conn));
        createUser();
    }

    // 建任务：默认 13 片 / 50MB，status=0；返回 task_id 并登记清理。
    std::string createTask(std::uint32_t totalChunks, std::uint64_t totalSize) {
        return DaoTestBase::createTask(m_uid, totalChunks, totalSize);
    }

    cm::db::ChunkWrite makeChunk(std::uint32_t index,
                                 std::uint32_t size,
                                 std::uint64_t offset) {
        cm::db::ChunkWrite chunk;
        chunk.index = index;
        chunk.offset = offset;
        chunk.size = size;
        chunk.md5 = nextHex32();
        char path[64];
        std::snprintf(path, sizeof(path), "tmp/chunk_%u.bin", index);
        chunk.path = std::string(path);
        return chunk;
    }

    // 直接写 c_state=0 的半成品行（模拟“写盘后进程崩溃”）。
    bool insertHalfChunk(const std::string& taskId, std::uint32_t index,
                         std::uint64_t offset, std::uint32_t size,
                         const std::string& md5) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "INSERT INTO t_chunk (c_task_id, c_index, c_offset, c_size, c_md5, "
            "c_state, c_path) VALUES (?, ?, ?, ?, ?, 0, 'tmp/half.bin')");
        if (!stmt) {
            return false;
        }
        return stmt->bindString(0, taskId) && stmt->bindUInt64(1, index) &&
               stmt->bindUInt64(2, offset) && stmt->bindUInt64(3, size) &&
               stmt->bindString(4, md5) && stmt->execute();
    }

    bool setTaskStatus(const std::string& taskId, std::uint8_t status) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "UPDATE t_transfer_task SET tr_status = ? WHERE tr_task_id = ?");
        if (!stmt) {
            return false;
        }
        return stmt->bindUInt64(0, status) && stmt->bindString(1, taskId) &&
               stmt->execute();
    }

    bool expireTaskNow(const std::string& taskId) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "UPDATE t_transfer_task SET tr_expire_at = DATE_SUB(NOW(6), INTERVAL 1 HOUR) "
            "WHERE tr_task_id = ?");
        if (!stmt) {
            return false;
        }
        return stmt->bindString(0, taskId) && stmt->execute();
    }

    bool readChunk(const std::string& taskId, std::uint32_t index,
                   std::string* md5, std::uint32_t* size, std::uint8_t* state) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
            "SELECT c_md5, c_size, c_state FROM t_chunk "
            "WHERE c_task_id = ? AND c_index = ?");
        if (!stmt || !stmt->bindString(0, taskId) || !stmt->bindUInt64(1, index)) {
            return false;
        }
        if (!stmt->execute()) {
            return false;
        }
        if (stmt->fetchRow() != cm::db::PreparedStatement::FetchStatus::Row) {
            return false;
        }
        *md5 = stmt->getString(0);
        *size = static_cast<std::uint32_t>(stmt->getUInt64(1));
        *state = static_cast<std::uint8_t>(stmt->getUInt64(2));
        return true;
    }

    cm::db::TransferTaskRow reload(const std::string& taskId) {
        cm::db::TransferTaskRow row;
        EXPECT_EQ(cm::ErrorCode::Ok, m_dao->findTaskByTaskId(taskId, &row));
        return row;
    }

    bool containsTask(const std::vector<cm::db::TransferTaskRow>& rows,
                      const std::string& taskId) {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].taskId == taskId) {
                return true;
            }
        }
        return false;
    }

    std::unique_ptr<cm::db::TransferDao> m_dao;
};

TEST_F(TransferDaoTest, TransferDaoCreateAndFind) {
    const std::string taskId = nextHex32();
    cm::db::NewTransferTask task;
    task.taskId = taskId;
    task.userId = m_uid;
    task.type = 1;
    task.fileMd5 = nextHex32();
    task.fileName = "phase5.bin";
    task.totalSize = 52428800;
    task.chunkSize = kChunkSize;
    task.totalChunks = 13;
    task.parentUfId = 0;
    task.ttlSeconds = 3600;

    std::uint64_t trId = 0;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->createTask(task, &trId));
    ASSERT_GT(trId, std::uint64_t(0));
    trackTask(taskId);

    cm::db::TransferTaskRow row;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findTaskByTaskId(taskId, &row));
    EXPECT_EQ(trId, row.trId);
    EXPECT_EQ(taskId, row.taskId);
    EXPECT_EQ(m_uid, row.userId);
    EXPECT_EQ(1, static_cast<int>(row.type));
    EXPECT_EQ(task.fileMd5, row.fileMd5);
    EXPECT_EQ(std::string("phase5.bin"), row.fileName);
    EXPECT_EQ(std::uint64_t(52428800), row.totalSize);
    EXPECT_EQ(kChunkSize, row.chunkSize);
    EXPECT_EQ(13u, row.totalChunks);
    EXPECT_EQ(0u, row.doneChunks);
    EXPECT_EQ(std::uint64_t(0), row.offset);
    EXPECT_EQ(0, static_cast<int>(row.status));
    EXPECT_EQ(0, row.errorCode);
    EXPECT_FALSE(row.expireAt.empty());
    // DATETIME(6) 文本可直接按字典序比较：ttl=3600s。
    EXPECT_LT(row.createdAt, row.expireAt);

    // 不存在：NotFound。
    cm::db::TransferTaskRow missing;
    EXPECT_EQ(cm::ErrorCode::NotFound,
              m_dao->findTaskByTaskId(nextHex32(), &missing));

    // 同一 task_id 再插一次：AlreadyExists（uk_task_id）。
    std::uint64_t again = 0;
    EXPECT_EQ(cm::ErrorCode::AlreadyExists, m_dao->createTask(task, &again));
    EXPECT_EQ(std::uint64_t(0), again);

    // file_md5 为空必须落成 NULL 并能读回空串（转存任务未必有 md5）。
    cm::db::NewTransferTask noMd5 = task;
    noMd5.taskId = nextHex32();
    noMd5.fileMd5.clear();
    std::uint64_t noMd5Id = 0;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->createTask(noMd5, &noMd5Id));
    trackTask(noMd5.taskId);
    cm::db::TransferTaskRow noMd5Row;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findTaskByTaskId(noMd5.taskId, &noMd5Row));
    EXPECT_TRUE(noMd5Row.fileMd5.empty());

    // lockTaskForUpdate 只读加锁：调用方开事务，用完回滚。
    {
        cm::db::Transaction tx(*m_conn);
        ASSERT_TRUE(tx.valid());
        cm::db::TransferTaskRow lockedRow;
        ASSERT_EQ(cm::ErrorCode::Ok,
                  m_dao->lockTaskForUpdate(m_uid, taskId, &lockedRow));
        EXPECT_EQ(trId, lockedRow.trId);
        EXPECT_EQ(cm::ErrorCode::NotFound,
                  m_dao->lockTaskForUpdate(m_uid + 100000000ULL, taskId, &lockedRow));
        tx.rollback();
    }

    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              m_dao->createTask(cm::db::NewTransferTask(), &again));
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              m_dao->findTaskByTaskId("short", &missing));
}

TEST_F(TransferDaoTest, TransferDaoUpsertChunkNew) {
    const std::string taskId = createTask(13, 52428800);
    ASSERT_FALSE(taskId.empty());

    const cm::db::ChunkWrite chunk0 = makeChunk(0, kChunkSize, 0);
    cm::db::ChunkUpsertResult result;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->upsertChunkIfAbsent(m_uid, taskId, chunk0, &result));
    EXPECT_EQ(cm::db::ChunkWriteKind::Inserted, result.kind);
    EXPECT_EQ(1u, result.doneChunks);
    EXPECT_EQ(std::uint64_t(kChunkSize), result.doneBytes);
    EXPECT_EQ(13u, result.totalChunks);
    EXPECT_EQ(1, static_cast<int>(result.status));

    cm::db::TransferTaskRow row = reload(taskId);
    EXPECT_EQ(1u, row.doneChunks);
    EXPECT_EQ(std::uint64_t(kChunkSize), row.offset);
    EXPECT_EQ(1, static_cast<int>(row.status));

    // 乱序：第 2 片先到，计数只 +1，offset 按片大小累加。
    const cm::db::ChunkWrite chunk2 =
        makeChunk(2, kChunkSize, static_cast<std::uint64_t>(kChunkSize) * 2);
    result = cm::db::ChunkUpsertResult();
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->upsertChunkIfAbsent(m_uid, taskId, chunk2, &result));
    EXPECT_EQ(cm::db::ChunkWriteKind::Inserted, result.kind);
    EXPECT_EQ(2u, result.doneChunks);
    EXPECT_EQ(std::uint64_t(kChunkSize) * 2, result.doneBytes);

    row = reload(taskId);
    EXPECT_EQ(2u, row.doneChunks);
    EXPECT_EQ(std::uint64_t(kChunkSize) * 2, row.offset);

    // 越界下标：拒绝，不落行。
    cm::db::ChunkUpsertResult ignored;
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              m_dao->upsertChunkIfAbsent(
                  m_uid, taskId, makeChunk(13, kChunkSize, 0), &ignored));
}

TEST_F(TransferDaoTest, TransferDaoUpsertChunkDuplicate) {
    const std::string taskId = createTask(13, 52428800);
    const cm::db::ChunkWrite chunk = makeChunk(0, kChunkSize, 0);

    cm::db::ChunkUpsertResult result;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->upsertChunkIfAbsent(m_uid, taskId, chunk, &result));
    ASSERT_EQ(cm::db::ChunkWriteKind::Inserted, result.kind);

    // 完全相同的重放：NoOp，计数与字节都不变。
    result = cm::db::ChunkUpsertResult();
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->upsertChunkIfAbsent(m_uid, taskId, chunk, &result));
    EXPECT_EQ(cm::db::ChunkWriteKind::NoOp, result.kind);
    EXPECT_EQ(1u, result.doneChunks);
    EXPECT_EQ(std::uint64_t(kChunkSize), result.doneBytes);

    cm::db::TransferTaskRow row = reload(taskId);
    EXPECT_EQ(1u, row.doneChunks);
    EXPECT_EQ(std::uint64_t(kChunkSize), row.offset);

    // 同 index 不同内容且已确认：Md5Mismatch，且不覆盖原分片。
    cm::db::ChunkWrite conflicting = chunk;
    conflicting.md5 = nextHex32();
    EXPECT_EQ(cm::ErrorCode::Md5Mismatch,
              m_dao->upsertChunkIfAbsent(m_uid, taskId, conflicting, &result));

    std::string md5;
    std::uint32_t size = 0;
    std::uint8_t state = 0;
    ASSERT_TRUE(readChunk(taskId, 0, &md5, &size, &state));
    EXPECT_EQ(chunk.md5, md5);
    EXPECT_EQ(kChunkSize, size);
    EXPECT_EQ(1, static_cast<int>(state));

    row = reload(taskId);
    EXPECT_EQ(1u, row.doneChunks);
    EXPECT_EQ(std::uint64_t(kChunkSize), row.offset);

    // 非本人：NotFound，不泄露任务存在性。
    EXPECT_EQ(cm::ErrorCode::NotFound,
              m_dao->upsertChunkIfAbsent(m_uid + 100000000ULL, taskId, chunk, &result));
}

TEST_F(TransferDaoTest, TransferDaoUpsertChunkRewriteState0) {
    const std::string taskId = createTask(13, 52428800);

    // 半成品行：上次写盘成功但进程在写 DB 前崩溃，计数不应包含它。
    const std::string halfMd5 = nextHex32();
    ASSERT_TRUE(insertHalfChunk(taskId, 1, kChunkSize, kChunkSize, halfMd5));

    cm::db::TransferTaskRow before = reload(taskId);
    EXPECT_EQ(0u, before.doneChunks);
    EXPECT_EQ(std::uint64_t(0), before.offset);

    const cm::db::ChunkWrite chunk = makeChunk(1, kChunkSize, kChunkSize);
    cm::db::ChunkUpsertResult result;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->upsertChunkIfAbsent(m_uid, taskId, chunk, &result));
    EXPECT_EQ(cm::db::ChunkWriteKind::Updated, result.kind);
    EXPECT_EQ(1u, result.doneChunks);
    EXPECT_EQ(std::uint64_t(kChunkSize), result.doneBytes);

    std::string md5;
    std::uint32_t size = 0;
    std::uint8_t state = 0;
    ASSERT_TRUE(readChunk(taskId, 1, &md5, &size, &state));
    EXPECT_EQ(chunk.md5, md5);
    EXPECT_EQ(kChunkSize, size);
    EXPECT_EQ(1, static_cast<int>(state));

    const cm::db::TransferTaskRow after = reload(taskId);
    EXPECT_EQ(1u, after.doneChunks);
    EXPECT_EQ(std::uint64_t(kChunkSize), after.offset);
}

TEST_F(TransferDaoTest, TransferDaoDoneIndexesBitmap) {
    const std::string taskId = createTask(10, static_cast<std::uint64_t>(kChunkSize) * 10);

    const std::uint32_t upsertIndexes[3] = {0, 3, 7};
    for (std::size_t i = 0; i < 3; ++i) {
        const std::uint32_t index = upsertIndexes[i];
        cm::db::ChunkUpsertResult result;
        ASSERT_EQ(cm::ErrorCode::Ok,
                  m_dao->upsertChunkIfAbsent(
                      m_uid, taskId,
                      makeChunk(index, kChunkSize,
                                static_cast<std::uint64_t>(index) * kChunkSize),
                      &result));
    }
    // state=0 的半成品不得出现在位图里。
    ASSERT_TRUE(insertHalfChunk(taskId, 5,
                                static_cast<std::uint64_t>(kChunkSize) * 5,
                                kChunkSize, nextHex32()));

    std::vector<std::uint32_t> indexes;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->listDoneIndexes(taskId, &indexes));
    ASSERT_EQ(std::size_t(3), indexes.size());
    EXPECT_EQ(0u, indexes[0]);
    EXPECT_EQ(3u, indexes[1]);
    EXPECT_EQ(7u, indexes[2]);

    std::uint64_t bytes = 0;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->sumDoneBytes(taskId, &bytes));
    EXPECT_EQ(std::uint64_t(kChunkSize) * 3, bytes);

    // 服务层按 LSB-first 组装：bit i = 第 i 片完成。
    std::uint8_t bitmap = 0;
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        bitmap = static_cast<std::uint8_t>(bitmap | (1u << indexes[i]));
    }
    EXPECT_EQ(0x89, static_cast<int>(bitmap));
    EXPECT_EQ(0, (bitmap >> 5) & 0x1);
}

TEST_F(TransferDaoTest, TransferDaoElectMergeOnce) {
    const std::string pending = createTask(2, static_cast<std::uint64_t>(kChunkSize) * 2);
    bool elected = false;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->electMerge(m_uid, pending, &elected));
    EXPECT_TRUE(elected);
    EXPECT_EQ(7, static_cast<int>(reload(pending).status));

    // 已被别人抢到：再选主失败，返回 Ok 但 elected=false。
    elected = true;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->electMerge(m_uid, pending, &elected));
    EXPECT_FALSE(elected);

    // 状态 1（上传中）同样可被选主。
    const std::string uploading =
        createTask(2, static_cast<std::uint64_t>(kChunkSize) * 2);
    ASSERT_TRUE(setTaskStatus(uploading, 1));
    elected = false;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->electMerge(m_uid, uploading, &elected));
    EXPECT_TRUE(elected);

    // 已取消（5）不可选主。
    const std::string cancelled =
        createTask(2, static_cast<std::uint64_t>(kChunkSize) * 2);
    ASSERT_TRUE(setTaskStatus(cancelled, 5));
    elected = true;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->electMerge(m_uid, cancelled, &elected));
    EXPECT_FALSE(elected);

    // 非本人：条件 UPDATE 命中 0 行，elected=false（不区分“不存在”与“无权”，由服务层读状态）。
    elected = true;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->electMerge(m_uid + 100000000ULL, pending, &elected));
    EXPECT_FALSE(elected);
}

TEST_F(TransferDaoTest, TransferDaoExpiredScan) {
    const std::string expiredUploading = createTask(2, 8192);
    const std::string expiredDone = createTask(2, 8192);
    const std::string fresh = createTask(2, 8192);

    ASSERT_TRUE(setTaskStatus(expiredUploading, 1));
    ASSERT_TRUE(setTaskStatus(expiredDone, 4));
    ASSERT_TRUE(expireTaskNow(expiredUploading));
    ASSERT_TRUE(expireTaskNow(expiredDone));

    std::vector<cm::db::TransferTaskRow> rows;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findExpired(&rows, 200));
    EXPECT_LE(rows.size(), std::size_t(200));
    EXPECT_TRUE(containsTask(rows, expiredUploading));
    // status=4（完成）即使过期也不能被扫出来。
    EXPECT_FALSE(containsTask(rows, expiredDone));
    // 未过期的不在结果里。
    EXPECT_FALSE(containsTask(rows, fresh));

    for (std::size_t i = 0; i < rows.size(); ++i) {
        EXPECT_TRUE(rows[i].status == 0 || rows[i].status == 1 || rows[i].status == 2);
    }

    // LIMIT 生效。
    std::vector<cm::db::TransferTaskRow> limited;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findExpired(&limited, 1));
    EXPECT_LE(limited.size(), std::size_t(1));

    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->findExpired(&limited, 0));
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->findExpired(&limited, 1001));
}

TEST_F(TransferDaoTest, TransferDaoResetMerging) {
    const std::string merging = createTask(2, 8192);
    const std::string uploading = createTask(2, 8192);
    ASSERT_TRUE(setTaskStatus(merging, 7));
    ASSERT_TRUE(setTaskStatus(uploading, 1));

    std::uint64_t affected = 0;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->resetMergingOnStartup(&affected));
    // 全局扫描：不能断言恰好 1（测试库可能残留其他 7）。
    EXPECT_GE(affected, std::uint64_t(1));

    EXPECT_EQ(2, static_cast<int>(reload(merging).status));
    EXPECT_EQ(1, static_cast<int>(reload(uploading).status));

    // markStatus 条件更新：状态不符时 changed=false，不改值。
    bool changed = true;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->markStatus(m_uid, merging, std::vector<std::uint8_t>(1, 2), 6, 0,
                                &changed));
    EXPECT_TRUE(changed);
    EXPECT_EQ(6, static_cast<int>(reload(merging).status));

    changed = true;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->markStatus(m_uid, merging,
                                std::vector<std::uint8_t>{0, 1, 2}, 6, 0, &changed));
    EXPECT_FALSE(changed);
    EXPECT_EQ(6, static_cast<int>(reload(merging).status));

    // 非本人：affected=0。
    changed = true;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->markStatus(m_uid + 100000000ULL, uploading,
                                std::vector<std::uint8_t>(1, 1), 3, 1005, &changed));
    EXPECT_FALSE(changed);
    EXPECT_EQ(1, static_cast<int>(reload(uploading).status));

    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              m_dao->markStatus(m_uid, uploading, std::vector<std::uint8_t>(), 6, 0,
                                &changed));
}

}  // namespace
