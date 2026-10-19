#include "transfer_dao.h"

#include <cctype>
#include <memory>

#include "mysql_conn.h"
#include "prepared_statement.h"

namespace cm {
namespace db {
namespace {

const std::size_t kTaskIdLength = 32;
const std::size_t kMaxTaskListLimit = 1000;
const std::uint32_t kDefaultTaskTtlSeconds = 86400;

// 任务列顺序必须与 fillTaskRow 的取值顺序一致。
const char kTaskColumns[] =
    "tr_id, tr_task_id, tr_user_id, tr_type, tr_file_md5, tr_file_name, "
    "tr_total_size, tr_chunk_size, tr_total_chunks, tr_done_chunks, tr_offset, "
    "tr_parent_uf_id, tr_status, tr_error_code, tr_expire_at, tr_created_at, "
    "tr_updated_at, "
    "CAST(UNIX_TIMESTAMP(tr_expire_at) * 1000 AS UNSIGNED)";

bool isDuplicateEntry(const std::string& message) {
    return message.find("Duplicate entry") != std::string::npos ||
           message.find("ER_DUP_ENTRY") != std::string::npos;
}

bool isHex32(const std::string& value) {
    if (value.size() != kTaskIdLength) {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (std::isxdigit(static_cast<unsigned char>(value[i])) == 0) {
            return false;
        }
    }
    return true;
}

void fillTaskRow(const PreparedStatement& stmt, TransferTaskRow* row) {
    row->trId = stmt.getUInt64(0);
    row->taskId = stmt.getString(1);
    row->userId = stmt.getUInt64(2);
    row->type = static_cast<std::uint8_t>(stmt.getUInt64(3));
    row->fileMd5 = stmt.isNull(4) ? std::string() : stmt.getString(4);
    row->fileName = stmt.getString(5);
    row->totalSize = stmt.getUInt64(6);
    row->chunkSize = static_cast<std::uint32_t>(stmt.getUInt64(7));
    row->totalChunks = static_cast<std::uint32_t>(stmt.getUInt64(8));
    row->doneChunks = static_cast<std::uint32_t>(stmt.getUInt64(9));
    row->offset = stmt.getUInt64(10);
    row->parentUfId = stmt.getUInt64(11);
    row->status = static_cast<std::uint8_t>(stmt.getUInt64(12));
    row->errorCode = static_cast<std::int32_t>(stmt.getInt64(13));
    row->expireAt = stmt.getString(14);
    row->createdAt = stmt.getString(15);
    row->updatedAt = stmt.getString(16);
    row->expireAtMs = stmt.getUInt64(17);
}

bool isWritableStatus(std::uint8_t status) {
    // 0 Pending / 1 Uploading / 2 Paused 都允许继续传（06 §5.1 允许的请求）。
    return status == 0 || status == 1 || status == 2;
}

}  // namespace

TransferTaskRow::TransferTaskRow()
    : trId(0),
      userId(0),
      type(0),
      totalSize(0),
      chunkSize(0),
      totalChunks(0),
      doneChunks(0),
      offset(0),
      parentUfId(0),
      status(0),
      errorCode(0),
      expireAtMs(0) {
}

NewTransferTask::NewTransferTask()
    : userId(0),
      type(0),
      totalSize(0),
      chunkSize(0),
      totalChunks(0),
      parentUfId(0),
      ttlSeconds(kDefaultTaskTtlSeconds) {
}

ChunkRow::ChunkRow()
    : index(0), offset(0), size(0), state(0) {
}

ChunkWrite::ChunkWrite() : index(0), offset(0), size(0) {
}

ChunkUpsertResult::ChunkUpsertResult()
    : kind(ChunkWriteKind::NoOp), doneChunks(0), doneBytes(0), totalChunks(0), status(0) {
}

TransferDao::TransferDao(MysqlConn& conn) : m_conn(conn) {
}

ErrorCode TransferDao::createTask(const NewTransferTask& task, std::uint64_t* trId) {
    if (trId == NULL || !isHex32(task.taskId) || task.userId == 0 ||
        task.type < 1 || task.type > 3 || task.fileName.empty() ||
        task.fileName.size() > 255 || task.totalSize == 0 || task.chunkSize == 0 ||
        task.totalChunks == 0 || task.ttlSeconds == 0) {
        return ErrorCode::InvalidArgument;
    }
    if (!task.fileMd5.empty() && !isHex32(task.fileMd5)) {
        return ErrorCode::InvalidArgument;
    }
    *trId = 0;

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "INSERT INTO t_transfer_task (tr_task_id, tr_user_id, tr_type, tr_file_md5, "
        "tr_file_name, tr_total_size, tr_chunk_size, tr_total_chunks, "
        "tr_parent_uf_id, tr_expire_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, DATE_ADD(NOW(6), INTERVAL ? SECOND))");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindString(0, task.taskId) || !stmt->bindUInt64(1, task.userId) ||
        !stmt->bindUInt64(2, task.type)) {
        return stmt->lastError();
    }
    if (task.fileMd5.empty()) {
        if (!stmt->bindNull(3)) {
            return stmt->lastError();
        }
    } else if (!stmt->bindString(3, task.fileMd5)) {
        return stmt->lastError();
    }
    if (!stmt->bindString(4, task.fileName) || !stmt->bindUInt64(5, task.totalSize) ||
        !stmt->bindUInt64(6, task.chunkSize) || !stmt->bindUInt64(7, task.totalChunks) ||
        !stmt->bindUInt64(8, task.parentUfId) ||
        !stmt->bindUInt64(9, task.ttlSeconds)) {
        return stmt->lastError();
    }
    if (!stmt->execute()) {
        if (isDuplicateEntry(stmt->lastErrorMessage())) {
            return ErrorCode::AlreadyExists;
        }
        return stmt->lastError();
    }

    *trId = stmt->insertId();
    return *trId == 0 ? ErrorCode::DbQueryFailed : ErrorCode::Ok;
}

ErrorCode TransferDao::findTaskByTaskId(const std::string& taskId,
                                        TransferTaskRow* out) {
    if (out == NULL || !isHex32(taskId)) {
        return ErrorCode::InvalidArgument;
    }

    std::string sql = "SELECT ";
    sql += kTaskColumns;
    sql += " FROM t_transfer_task WHERE tr_task_id = ?";

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(sql);
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindString(0, taskId)) {
        return stmt->lastError();
    }
    if (!stmt->execute()) {
        return stmt->lastError();
    }

    const PreparedStatement::FetchStatus status = stmt->fetchRow();
    if (status == PreparedStatement::FetchStatus::End) {
        return ErrorCode::NotFound;
    }
    if (status == PreparedStatement::FetchStatus::Error) {
        return stmt->lastError();
    }
    fillTaskRow(*stmt, out);
    return ErrorCode::Ok;
}

ErrorCode TransferDao::lockTaskForUpdate(std::uint64_t uid,
                                         const std::string& taskId,
                                         TransferTaskRow* out) {
    if (out == NULL || uid == 0 || !isHex32(taskId)) {
        return ErrorCode::InvalidArgument;
    }

    std::string sql = "SELECT ";
    sql += kTaskColumns;
    sql += " FROM t_transfer_task WHERE tr_task_id = ? AND tr_user_id = ? FOR UPDATE";

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(sql);
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindString(0, taskId) || !stmt->bindUInt64(1, uid)) {
        return stmt->lastError();
    }
    if (!stmt->execute()) {
        return stmt->lastError();
    }

    const PreparedStatement::FetchStatus status = stmt->fetchRow();
    if (status == PreparedStatement::FetchStatus::End) {
        // 不存在与非本人同样回 NotFound，不泄露任务存在性（06 §4.2）。
        return ErrorCode::NotFound;
    }
    if (status == PreparedStatement::FetchStatus::Error) {
        return stmt->lastError();
    }
    fillTaskRow(*stmt, out);
    return ErrorCode::Ok;
}

ErrorCode TransferDao::upsertChunkIfAbsent(std::uint64_t uid,
                                           const std::string& taskId,
                                           const ChunkWrite& chunk,
                                           ChunkUpsertResult* out) {
    if (out == NULL || uid == 0 || !isHex32(taskId) || !isHex32(chunk.md5) ||
        chunk.size == 0 || chunk.path.empty()) {
        return ErrorCode::InvalidArgument;
    }
    *out = ChunkUpsertResult();

    Transaction tx(m_conn);
    if (!tx.valid()) {
        return m_conn.lastError();
    }

    // 锁顺序：任务行 → 分片行（与合并落库事务一致，禁止反向）。
    TransferTaskRow task;
    const ErrorCode locked = lockTaskForUpdate(uid, taskId, &task);
    if (locked != ErrorCode::Ok) {
        return locked;
    }
    if (!isWritableStatus(task.status)) {
        return ErrorCode::TaskStateInvalid;
    }
    if (chunk.index >= task.totalChunks) {
        return ErrorCode::InvalidArgument;
    }

    std::unique_ptr<PreparedStatement> select = m_conn.prepare(
        "SELECT c_md5, c_state FROM t_chunk WHERE c_task_id = ? AND c_index = ? "
        "FOR UPDATE");
    if (!select) {
        return m_conn.lastError();
    }
    if (!select->bindString(0, taskId) || !select->bindUInt64(1, chunk.index)) {
        return select->lastError();
    }
    if (!select->execute()) {
        return select->lastError();
    }

    const PreparedStatement::FetchStatus fetched = select->fetchRow();
    if (fetched == PreparedStatement::FetchStatus::Error) {
        return select->lastError();
    }

    ChunkWriteKind kind = ChunkWriteKind::Inserted;
    if (fetched == PreparedStatement::FetchStatus::Row) {
        const std::string existingMd5 = select->getString(0);
        const std::uint8_t existingState =
            static_cast<std::uint8_t>(select->getUInt64(1));
        select.reset();

        if (existingState == 1) {
            if (existingMd5 != chunk.md5) {
                // 已确认分片不允许被不同内容覆盖。
                return ErrorCode::Md5Mismatch;
            }
            // 完全相同的重放：不计数、不累加、不续期。
            if (!tx.commit()) {
                return tx.lastError();
            }
            out->kind = ChunkWriteKind::NoOp;
            out->doneChunks = task.doneChunks;
            out->doneBytes = task.offset;
            out->totalChunks = task.totalChunks;
            out->status = task.status;
            return ErrorCode::Ok;
        }

        // c_state=0：上次写盘后进程崩溃留下的半成品，允许覆盖并计为一次新完成。
        std::unique_ptr<PreparedStatement> update = m_conn.prepare(
            "UPDATE t_chunk SET c_offset = ?, c_size = ?, c_md5 = ?, c_state = 1, "
            "c_path = ? WHERE c_task_id = ? AND c_index = ?");
        if (!update) {
            return m_conn.lastError();
        }
        if (!update->bindUInt64(0, chunk.offset) || !update->bindUInt64(1, chunk.size) ||
            !update->bindString(2, chunk.md5) ||
            !update->bindString(3, chunk.path) ||
            !update->bindString(4, taskId) || !update->bindUInt64(5, chunk.index)) {
            return update->lastError();
        }
        if (!update->execute()) {
            return update->lastError();
        }
        kind = ChunkWriteKind::Updated;
    } else {
        select.reset();
        std::unique_ptr<PreparedStatement> insert = m_conn.prepare(
            "INSERT INTO t_chunk (c_task_id, c_index, c_offset, c_size, c_md5, "
            "c_state, c_path) VALUES (?, ?, ?, ?, ?, 1, ?)");
        if (!insert) {
            return m_conn.lastError();
        }
        if (!insert->bindString(0, taskId) || !insert->bindUInt64(1, chunk.index) ||
            !insert->bindUInt64(2, chunk.offset) ||
            !insert->bindUInt64(3, chunk.size) || !insert->bindString(4, chunk.md5) ||
            !insert->bindString(5, chunk.path)) {
            return insert->lastError();
        }
        if (!insert->execute()) {
            return insert->lastError();
        }
        kind = ChunkWriteKind::Inserted;
    }

    // 只为新完成的分片 +1 / 累加 c_size；首片把 0(Pending) 推进到 1(Uploading)，
    // 同时按 06 §5.7 续期：LEAST(创建+7d, NOW+24h)。
    std::unique_ptr<PreparedStatement> taskUpdate = m_conn.prepare(
        "UPDATE t_transfer_task SET tr_done_chunks = tr_done_chunks + 1, "
        "tr_offset = tr_offset + ?, "
        "tr_status = CASE WHEN tr_status = 0 THEN 1 ELSE tr_status END, "
        "tr_expire_at = LEAST(DATE_ADD(tr_created_at, INTERVAL 7 DAY), "
        "DATE_ADD(NOW(6), INTERVAL 24 HOUR)) "
        "WHERE tr_task_id = ? AND tr_user_id = ?");
    if (!taskUpdate) {
        return m_conn.lastError();
    }
    if (!taskUpdate->bindUInt64(0, chunk.size) ||
        !taskUpdate->bindString(1, taskId) || !taskUpdate->bindUInt64(2, uid)) {
        return taskUpdate->lastError();
    }
    if (!taskUpdate->execute()) {
        return taskUpdate->lastError();
    }

    if (!tx.commit()) {
        return tx.lastError();
    }

    out->kind = kind;
    out->doneChunks = task.doneChunks + 1;
    out->doneBytes = task.offset + chunk.size;
    out->totalChunks = task.totalChunks;
    out->status = (task.status == 0) ? 1 : task.status;
    return ErrorCode::Ok;
}

ErrorCode TransferDao::findChunk(const std::string& taskId,
                                   std::uint32_t index,
                                   ChunkRow* out) {
    if (out == NULL || !isHex32(taskId)) {
        return ErrorCode::InvalidArgument;
    }

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "SELECT c_index, c_offset, c_size, c_md5, c_state, c_path "
        "FROM t_chunk WHERE c_task_id = ? AND c_index = ?");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindString(0, taskId) || !stmt->bindUInt64(1, index)) {
        return stmt->lastError();
    }
    if (!stmt->execute()) {
        return stmt->lastError();
    }

    const PreparedStatement::FetchStatus status = stmt->fetchRow();
    if (status == PreparedStatement::FetchStatus::End) {
        return ErrorCode::NotFound;
    }
    if (status == PreparedStatement::FetchStatus::Error) {
        return stmt->lastError();
    }

    out->index = static_cast<std::uint32_t>(stmt->getUInt64(0));
    out->offset = stmt->getUInt64(1);
    out->size = static_cast<std::uint32_t>(stmt->getUInt64(2));
    out->md5 = stmt->getString(3);
    out->state = static_cast<std::uint8_t>(stmt->getUInt64(4));
    out->path = stmt->getString(5);
    return ErrorCode::Ok;
}

ErrorCode TransferDao::listDoneIndexes(const std::string& taskId,
                                       std::vector<std::uint32_t>* out) {
    if (out == NULL || !isHex32(taskId)) {
        return ErrorCode::InvalidArgument;
    }
    out->clear();

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "SELECT c_index FROM t_chunk WHERE c_task_id = ? AND c_state = 1 "
        "ORDER BY c_index ASC");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindString(0, taskId)) {
        return stmt->lastError();
    }
    if (!stmt->execute()) {
        return stmt->lastError();
    }

    for (;;) {
        const PreparedStatement::FetchStatus status = stmt->fetchRow();
        if (status == PreparedStatement::FetchStatus::End) {
            break;
        }
        if (status == PreparedStatement::FetchStatus::Error) {
            return stmt->lastError();
        }
        out->push_back(static_cast<std::uint32_t>(stmt->getUInt64(0)));
    }
    return ErrorCode::Ok;
}

ErrorCode TransferDao::listChunks(const std::string& taskId,
                                 std::vector<ChunkRow>* out) {
    if (out == NULL || !isHex32(taskId)) {
        return ErrorCode::InvalidArgument;
    }
    out->clear();

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "SELECT c_index, c_offset, c_size, c_md5, c_state, c_path "
        "FROM t_chunk WHERE c_task_id = ? ORDER BY c_index ASC");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindString(0, taskId)) {
        return stmt->lastError();
    }
    if (!stmt->execute()) {
        return stmt->lastError();
    }

    for (;;) {
        const PreparedStatement::FetchStatus status = stmt->fetchRow();
        if (status == PreparedStatement::FetchStatus::End) {
            break;
        }
        if (status == PreparedStatement::FetchStatus::Error) {
            return stmt->lastError();
        }
        ChunkRow row;
        row.index = static_cast<std::uint32_t>(stmt->getUInt64(0));
        row.offset = stmt->getUInt64(1);
        row.size = static_cast<std::uint32_t>(stmt->getUInt64(2));
        row.md5 = stmt->getString(3);
        row.state = static_cast<std::uint8_t>(stmt->getUInt64(4));
        row.path = stmt->getString(5);
        out->push_back(row);
    }
    return ErrorCode::Ok;
}

ErrorCode TransferDao::sumDoneBytes(const std::string& taskId,
                                    std::uint64_t* bytes) {
    if (bytes == NULL || !isHex32(taskId)) {
        return ErrorCode::InvalidArgument;
    }
    *bytes = 0;

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "SELECT COALESCE(SUM(c_size), 0) FROM t_chunk "
        "WHERE c_task_id = ? AND c_state = 1");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindString(0, taskId)) {
        return stmt->lastError();
    }
    if (!stmt->execute()) {
        return stmt->lastError();
    }

    const PreparedStatement::FetchStatus status = stmt->fetchRow();
    if (status != PreparedStatement::FetchStatus::Row) {
        return status == PreparedStatement::FetchStatus::End
                   ? ErrorCode::DbQueryFailed
                   : stmt->lastError();
    }
    *bytes = stmt->getUInt64(0);
    return ErrorCode::Ok;
}

ErrorCode TransferDao::syncDoneCounters(const std::string& taskId,
                                        std::uint32_t doneChunks,
                                        std::uint64_t doneBytes) {
    if (!isHex32(taskId)) {
        return ErrorCode::InvalidArgument;
    }

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "UPDATE t_transfer_task SET tr_done_chunks = ?, tr_offset = ? "
        "WHERE tr_task_id = ?");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindUInt64(0, doneChunks) || !stmt->bindUInt64(1, doneBytes) ||
        !stmt->bindString(2, taskId)) {
        return stmt->lastError();
    }
    if (!stmt->execute()) {
        return stmt->lastError();
    }
    return ErrorCode::Ok;
}

ErrorCode TransferDao::markStatus(std::uint64_t uid,
                                  const std::string& taskId,
                                  const std::vector<std::uint8_t>& allowedFrom,
                                  std::uint8_t newStatus,
                                  std::int32_t errorCode,
                                  bool* changed) {
    if (changed == NULL || uid == 0 || !isHex32(taskId) || allowedFrom.empty() ||
        newStatus > 7) {
        return ErrorCode::InvalidArgument;
    }
    *changed = false;

    std::string sql =
        "UPDATE t_transfer_task SET tr_status = ?, tr_error_code = ? "
        "WHERE tr_task_id = ? AND tr_user_id = ? AND tr_status IN (";
    sql += PreparedStatement::placeholders(allowedFrom.size());
    sql += ")";

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(sql);
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindUInt64(0, newStatus) ||
        !stmt->bindInt64(1, static_cast<std::int64_t>(errorCode)) ||
        !stmt->bindString(2, taskId) || !stmt->bindUInt64(3, uid)) {
        return stmt->lastError();
    }
    for (std::size_t i = 0; i < allowedFrom.size(); ++i) {
        if (!stmt->bindUInt64(4 + i, allowedFrom[i])) {
            return stmt->lastError();
        }
    }
    if (!stmt->execute()) {
        return stmt->lastError();
    }

    *changed = stmt->affectedRows() > 0;
    return ErrorCode::Ok;
}

ErrorCode TransferDao::electMerge(std::uint64_t uid,
                                  const std::string& taskId,
                                  bool* elected) {
    if (elected == NULL || uid == 0 || !isHex32(taskId)) {
        return ErrorCode::InvalidArgument;
    }
    *elected = false;

    // 一条条件 UPDATE 完成选举：affected_rows=1 者合并，天然幂等（06 §5.5）。
    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "UPDATE t_transfer_task SET tr_status = 7 "
        "WHERE tr_task_id = ? AND tr_user_id = ? AND tr_status IN (0, 1)");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindString(0, taskId) || !stmt->bindUInt64(1, uid)) {
        return stmt->lastError();
    }
    if (!stmt->execute()) {
        return stmt->lastError();
    }

    *elected = stmt->affectedRows() == 1;
    return ErrorCode::Ok;
}

ErrorCode TransferDao::resetMergingOnStartup(std::uint64_t* affected) {
    if (affected == NULL) {
        return ErrorCode::InvalidArgument;
    }
    *affected = 0;

    std::unique_ptr<PreparedStatement> stmt =
        m_conn.prepare("UPDATE t_transfer_task SET tr_status = 2 WHERE tr_status = 7");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->execute()) {
        return stmt->lastError();
    }

    *affected = stmt->affectedRows();
    return ErrorCode::Ok;
}

ErrorCode TransferDao::findExpired(std::vector<TransferTaskRow>* out,
                                   std::size_t limit) {
    if (out == NULL || limit == 0 || limit > kMaxTaskListLimit) {
        return ErrorCode::InvalidArgument;
    }
    out->clear();

    std::string sql = "SELECT ";
    sql += kTaskColumns;
    sql +=
        " FROM t_transfer_task WHERE tr_status IN (0, 1, 2) "
        "AND tr_expire_at < NOW(6) ORDER BY tr_expire_at LIMIT ?";

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(sql);
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindUInt64(0, static_cast<std::uint64_t>(limit))) {
        return stmt->lastError();
    }
    if (!stmt->execute()) {
        return stmt->lastError();
    }

    for (;;) {
        const PreparedStatement::FetchStatus status = stmt->fetchRow();
        if (status == PreparedStatement::FetchStatus::End) {
            break;
        }
        if (status == PreparedStatement::FetchStatus::Error) {
            return stmt->lastError();
        }
        TransferTaskRow row;
        fillTaskRow(*stmt, &row);
        out->push_back(row);
    }
    return ErrorCode::Ok;
}

}  // namespace db
}  // namespace cm
