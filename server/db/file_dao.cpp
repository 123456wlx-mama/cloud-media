#include "file_dao.h"

#include <cstdio>
#include <memory>

#include "mysql_conn.h"
#include "prepared_statement.h"

namespace cm {
namespace db {
namespace {

const char kActiveDeletedAt[] = "1970-01-01 00:00:00.000000";
const std::size_t kMaxListLimit = 200;
const int kMaxRenameAttempt = 99;

// MySQL 3024：语句执行时间超过 MAX_EXECUTION_TIME 提示。
bool isExecutionTimeout(const std::string& message) {
    return message.find("maximum statement execution time") != std::string::npos ||
           message.find("Query execution was interrupted") != std::string::npos;
}

bool isDuplicateEntry(const std::string& message) {
    return message.find("Duplicate entry") != std::string::npos ||
           message.find("ER_DUP_ENTRY") != std::string::npos;
}

// "a.txt" + 2 -> "a (2).txt"；无扩展名或点开头（.bashrc）时整个名字当主干。
std::string candidateName(const std::string& name, int attempt) {
    if (attempt <= 1) {
        return name;
    }
    const std::string::size_type dot = name.find_last_of('.');
    const bool hasExtension =
        dot != std::string::npos && dot != 0 && dot + 1 < name.size();
    const std::string base = hasExtension ? name.substr(0, dot) : name;
    const std::string extension = hasExtension ? name.substr(dot) : std::string();

    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), " (%d)", attempt);
    return base + suffix + extension;
}

void fillUserFileRow(const PreparedStatement& stmt, UserFileRow* row) {
    row->ufId = stmt.getUInt64(0);
    row->uid = stmt.getUInt64(1);
    row->parentId = stmt.getUInt64(2);
    row->fid = stmt.isNull(3) ? 0 : stmt.getUInt64(3);
    row->name = stmt.getString(4);
    row->isDir = stmt.getUInt64(5) != 0;
    row->deletedAt = stmt.getString(6);
    row->createdAt = stmt.getString(7);
    row->updatedAt = stmt.getString(8);
}

// 行锁；锁顺序固定为“节点行 → blob 行”，与上传合并事务一致。
ErrorCode lockUserFile(MysqlConn& conn,
                       std::uint64_t uid,
                       std::uint64_t ufId,
                       std::uint64_t* fid,
                       bool* isDir,
                       std::string* deletedAt,
                       std::string* name) {
    std::unique_ptr<PreparedStatement> stmt = conn.prepare(
        "SELECT f_id, uf_is_dir, uf_deleted_at, uf_name FROM t_user_file "
        "WHERE uf_id = ? AND u_id = ? FOR UPDATE");
    if (!stmt) {
        return conn.lastError();
    }
    if (!stmt->bindUInt64(0, ufId) || !stmt->bindUInt64(1, uid)) {
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

    *fid = stmt->isNull(0) ? 0 : stmt->getUInt64(0);
    *isDir = stmt->getUInt64(1) != 0;
    *deletedAt = stmt->getString(2);
    if (name != NULL) {
        *name = stmt->getString(3);
    }
    return ErrorCode::Ok;
}

// 减到 0 只置孤儿，不 unlink：GC 任务在 24h 宽限后回收 blob 与 .crc。
ErrorCode decrementBlobRef(MysqlConn& conn, std::uint64_t fid, bool* orphaned) {
    if (orphaned != NULL) {
        *orphaned = false;
    }
    std::unique_ptr<PreparedStatement> drop = conn.prepare(
        "UPDATE t_file SET f_ref_count = f_ref_count - 1 "
        "WHERE f_id = ? AND f_ref_count > 0");
    if (!drop) {
        return conn.lastError();
    }
    if (!drop->bindUInt64(0, fid)) {
        return drop->lastError();
    }
    if (!drop->execute()) {
        return drop->lastError();
    }

    std::unique_ptr<PreparedStatement> orphan = conn.prepare(
        "UPDATE t_file SET f_state = 2 WHERE f_id = ? AND f_ref_count = 0");
    if (!orphan) {
        return conn.lastError();
    }
    if (!orphan->bindUInt64(0, fid)) {
        return orphan->lastError();
    }
    if (!orphan->execute()) {
        return orphan->lastError();
    }
    if (orphaned != NULL) {
        *orphaned = orphan->affectedRows() > 0;
    }
    return ErrorCode::Ok;
}

ErrorCode incrementBlobRef(MysqlConn& conn, std::uint64_t fid) {
    std::unique_ptr<PreparedStatement> stmt = conn.prepare(
        "UPDATE t_file SET f_ref_count = f_ref_count + 1, "
        "f_state = CASE WHEN f_state = 2 THEN 1 ELSE f_state END WHERE f_id = ?");
    if (!stmt) {
        return conn.lastError();
    }
    if (!stmt->bindUInt64(0, fid)) {
        return stmt->lastError();
    }
    if (!stmt->execute()) {
        return stmt->lastError();
    }
    return ErrorCode::Ok;
}

}  // namespace

BlobRow::BlobRow()
    : fid(0), size(0), refCount(0), state(0) {
}

UserFileRow::UserFileRow()
    : ufId(0), uid(0), parentId(0), fid(0), isDir(false), size(0) {
}

const std::uint64_t FileDao::kUnknownCount;

FileListQuery::FileListQuery()
    : uid(0), parentId(0), sort(FileListSort::NameAsc), limit(0), cursorId(0) {
}

FileListPage::FileListPage() : hasMore(false), nextId(0) {
}

RestoreOutcome::RestoreOutcome() : fid(0), alreadyRestored(false) {
}

RecycleNode::RecycleNode()
    : ufId(0), fid(0), isDir(false), parentId(0), deletedAtMs(0), size(0) {
}

RecycleListQuery::RecycleListQuery()
    : uid(0), limit(0), cursorDeletedMs(0), cursorId(0) {
}

RecycleListPage::RecycleListPage()
    : hasMore(false), nextDeletedMs(0), nextId(0) {
}

PurgeBatchResult::PurgeBatchResult()
    : filesDeleted(0), dirsDeleted(0), blobsOrphaned(0) {
}

FileDao::FileDao(MysqlConn& conn) : m_conn(conn) {
}

const char* FileDao::activeDeletedAt() {
    return kActiveDeletedAt;
}

ErrorCode FileDao::findBlobByMd5(const std::string& md5, BlobRow* out) {
    if (out == NULL || md5.size() != 32) {
        return ErrorCode::InvalidArgument;
    }

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "SELECT f_id, f_md5, f_size, f_path, f_ref_count, f_state "
        "FROM t_file WHERE f_md5 = ?");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindString(0, md5)) {
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

    out->fid = stmt->getUInt64(0);
    out->md5 = stmt->getString(1);
    out->size = stmt->getUInt64(2);
    out->path = stmt->getString(3);
    out->refCount = static_cast<std::uint32_t>(stmt->getUInt64(4));
    out->state = static_cast<std::uint8_t>(stmt->getUInt64(5));
    return ErrorCode::Ok;
}

ErrorCode FileDao::getBlobById(std::uint64_t fid, BlobRow* out) {
    if (out == NULL || fid == 0) {
        return ErrorCode::InvalidArgument;
    }

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "SELECT f_id, f_md5, f_size, f_path, f_ref_count, f_state "
        "FROM t_file WHERE f_id = ?");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindUInt64(0, fid)) {
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

    out->fid = stmt->getUInt64(0);
    out->md5 = stmt->getString(1);
    out->size = stmt->getUInt64(2);
    out->path = stmt->getString(3);
    out->refCount = static_cast<std::uint32_t>(stmt->getUInt64(4));
    out->state = static_cast<std::uint8_t>(stmt->getUInt64(5));
    return ErrorCode::Ok;
}

ErrorCode FileDao::insertBlobOnDuplicateIncrement(const std::string& md5,
                                                  std::uint64_t size,
                                                  const std::string& path,
                                                  std::uint64_t* fid,
                                                  BlobWriteKind* kind) {
    if (fid == NULL || kind == NULL || md5.size() != 32 || size == 0 || path.empty()) {
        return ErrorCode::InvalidArgument;
    }
    *fid = 0;
    *kind = BlobWriteKind::Inserted;

    // LAST_INSERT_ID(f_id) 让命中唯一键时 insertId() 仍返回已有 blob 的 f_id。
    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "INSERT INTO t_file (f_md5, f_size, f_path, f_ref_count, f_state) "
        "VALUES (?, ?, ?, 1, 1) "
        "ON DUPLICATE KEY UPDATE f_id = LAST_INSERT_ID(f_id), "
        "f_ref_count = f_ref_count + 1, f_state = 1, f_updated_at = NOW(6)");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindString(0, md5) || !stmt->bindUInt64(1, size) ||
        !stmt->bindString(2, path)) {
        return stmt->lastError();
    }
    if (!stmt->execute()) {
        return stmt->lastError();
    }

    const std::uint64_t affected = stmt->affectedRows();
    if (affected == 1) {
        *kind = BlobWriteKind::Inserted;
    } else {
        // affected_rows=2：命中 uk_f_md5 并 +1；=0 只在未开 CLIENT_FOUND_ROWS
        // 且值未变化时出现，本语句必然 +1，按命中处理不会误判。
        *kind = BlobWriteKind::RefIncremented;
    }

    const std::uint64_t blobId = stmt->insertId();
    if (blobId == 0) {
        return ErrorCode::DbQueryFailed;
    }
    *fid = blobId;
    return ErrorCode::Ok;
}

ErrorCode FileDao::createUserFileAutoRename(std::uint64_t uid,
                                            std::uint64_t parentId,
                                            std::uint64_t fid,
                                            bool isDir,
                                            const std::string& name,
                                            std::uint64_t* ufId,
                                            std::string* finalName) {
    if (ufId == NULL || finalName == NULL || uid == 0 || name.empty() ||
        name.size() > 255) {
        return ErrorCode::InvalidArgument;
    }
    if (isDir ? fid != 0 : fid == 0) {
        return ErrorCode::InvalidArgument;
    }
    *ufId = 0;
    finalName->clear();

    Transaction tx(m_conn);
    if (!tx.valid()) {
        return m_conn.lastError();
    }

    const ErrorCode rc =
        insertNodeWithRename(uid, parentId, fid, isDir, name, ufId, finalName);
    if (rc != ErrorCode::Ok) {
        return rc;
    }
    if (!tx.commit()) {
        return tx.lastError();
    }
    return ErrorCode::Ok;
}

ErrorCode FileDao::insertUserFileAutoRenameInTx(std::uint64_t uid,
                                               std::uint64_t parentId,
                                               std::uint64_t fid,
                                               const std::string& name,
                                               std::uint64_t* ufId,
                                               std::string* finalName) {
    if (ufId == NULL || finalName == NULL || uid == 0 || fid == 0 ||
        name.empty() || name.size() > 255) {
        return ErrorCode::InvalidArgument;
    }
    return insertNodeWithRename(uid, parentId, fid, false, name, ufId,
                                finalName);
}

ErrorCode FileDao::findActiveNodeByFid(std::uint64_t uid,
                                       std::uint64_t parentId,
                                       std::uint64_t fid,
                                       UserFileRow* out) {
    if (out == NULL || uid == 0 || fid == 0) {
        return ErrorCode::InvalidArgument;
    }

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "SELECT uf_id, u_id, uf_parent_id, f_id, uf_name, uf_is_dir, "
        "uf_deleted_at, uf_created_at, uf_updated_at "
        "FROM t_user_file WHERE u_id = ? AND uf_parent_id = ? AND f_id = ? "
        "AND uf_deleted_at = ? ORDER BY uf_id DESC LIMIT 1");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindUInt64(0, uid) || !stmt->bindUInt64(1, parentId) ||
        !stmt->bindUInt64(2, fid) || !stmt->bindString(3, kActiveDeletedAt)) {
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
    fillUserFileRow(*stmt, out);
    return ErrorCode::Ok;
}

ErrorCode FileDao::hasVisibleFileByFid(std::uint64_t uid,
                                       std::uint64_t fid,
                                       bool* visible) {
    if (visible == NULL || uid == 0 || fid == 0) {
        return ErrorCode::InvalidArgument;
    }
    *visible = false;

    // idx_file (f_id, u_id) 覆盖；只判存在，不取行。
    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "SELECT 1 FROM t_user_file "
        "WHERE u_id = ? AND f_id = ? AND uf_deleted_at = ? LIMIT 1");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindUInt64(0, uid) || !stmt->bindUInt64(1, fid) ||
        !stmt->bindString(2, kActiveDeletedAt)) {
        return stmt->lastError();
    }
    if (!stmt->execute()) {
        return stmt->lastError();
    }

    const PreparedStatement::FetchStatus status = stmt->fetchRow();
    if (status == PreparedStatement::FetchStatus::Error) {
        return stmt->lastError();
    }
    *visible = (status == PreparedStatement::FetchStatus::Row);
    return ErrorCode::Ok;
}

ErrorCode FileDao::insertNodeWithRename(std::uint64_t uid,
                                       std::uint64_t parentId,
                                       std::uint64_t fid,
                                       bool isDir,
                                       const std::string& name,
                                       std::uint64_t* ufId,
                                       std::string* finalName) {
    for (int attempt = 1; attempt <= kMaxRenameAttempt; ++attempt) {
        const std::string candidate = candidateName(name, attempt);

        std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
            "INSERT INTO t_user_file (u_id, uf_parent_id, f_id, uf_name, uf_is_dir) "
            "VALUES (?, ?, ?, ?, ?)");
        if (!stmt) {
            return m_conn.lastError();
        }
        if (!stmt->bindUInt64(0, uid) || !stmt->bindUInt64(1, parentId)) {
            return stmt->lastError();
        }
        if (isDir) {
            if (!stmt->bindNull(2)) {
                return stmt->lastError();
            }
        } else if (!stmt->bindUInt64(2, fid)) {
            return stmt->lastError();
        }
        if (!stmt->bindString(3, candidate) ||
            !stmt->bindUInt64(4, static_cast<std::uint64_t>(isDir ? 1 : 0))) {
            return stmt->lastError();
        }

        if (stmt->execute()) {
            *ufId = stmt->insertId();
            *finalName = candidate;
            return ErrorCode::Ok;
        }
        if (!isDuplicateEntry(stmt->lastErrorMessage())) {
            return stmt->lastError();
        }
        // 1062 是语句级回滚，事务仍可用；换下一个候选名继续。
    }
    return ErrorCode::AlreadyExists;
}

ErrorCode FileDao::materializeInstantUpload(std::uint64_t uid,
                                             std::uint64_t parentId,
                                             const std::string& md5,
                                             std::uint64_t size,
                                             const std::string& name,
                                             std::uint64_t* fileId,
                                             std::uint64_t* ufId,
                                             std::string* finalName,
                                             bool* reused) {
    if (fileId == NULL || ufId == NULL || finalName == NULL || reused == NULL ||
        uid == 0 || md5.size() != 32 || size == 0 || name.empty() ||
        name.size() > 255) {
        return ErrorCode::InvalidArgument;
    }
    *fileId = 0;
    *ufId = 0;
    finalName->clear();
    *reused = false;

    Transaction tx(m_conn);
    if (!tx.valid()) {
        return m_conn.lastError();
    }

    // 1) 同目录同名同 blob 的活跃节点已存在：复用，不重复加计数（客户端重试幂等）。
    BlobRow blob;
    if (findBlobByMd5(md5, &blob) == ErrorCode::Ok) {
        std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
            "SELECT uf_id FROM t_user_file WHERE u_id = ? AND uf_parent_id = ? "
            "AND f_id = ? AND uf_name = ? AND uf_deleted_at = ?");
        if (!stmt) {
            return m_conn.lastError();
        }
        if (!stmt->bindUInt64(0, uid) || !stmt->bindUInt64(1, parentId) ||
            !stmt->bindUInt64(2, blob.fid) || !stmt->bindString(3, name) ||
            !stmt->bindString(4, kActiveDeletedAt)) {
            return stmt->lastError();
        }
        if (!stmt->execute()) {
            return stmt->lastError();
        }

        const PreparedStatement::FetchStatus status = stmt->fetchRow();
        if (status == PreparedStatement::FetchStatus::Error) {
            return stmt->lastError();
        }
        if (status == PreparedStatement::FetchStatus::Row) {
            *fileId = blob.fid;
            *ufId = stmt->getUInt64(0);
            *finalName = name;
            *reused = true;
            if (!tx.commit()) {
                return tx.lastError();
            }
            return ErrorCode::Ok;
        }
    }

    // 2) 确保 blob 存在并把引用计数 +1。
    BlobWriteKind kind = BlobWriteKind::Inserted;
    const ErrorCode wrote = insertBlobOnDuplicateIncrement(
        md5, size, "aa/bb/" + md5, fileId, &kind);
    if (wrote != ErrorCode::Ok) {
        return wrote;
    }

    // 3) 插入用户节点（同名自动改名）。
    const ErrorCode inserted =
        insertNodeWithRename(uid, parentId, *fileId, false, name, ufId, finalName);
    if (inserted != ErrorCode::Ok) {
        return inserted;
    }

    if (!tx.commit()) {
        return tx.lastError();
    }
    return ErrorCode::Ok;
}

ErrorCode FileDao::findUserFile(std::uint64_t uid,
                                std::uint64_t ufId,
                                UserFileRow* out) {
    if (out == NULL || uid == 0 || ufId == 0) {
        return ErrorCode::InvalidArgument;
    }

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "SELECT uf_id, u_id, uf_parent_id, f_id, uf_name, uf_is_dir, "
        "uf_deleted_at, uf_created_at, uf_updated_at "
        "FROM t_user_file WHERE uf_id = ? AND u_id = ?");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindUInt64(0, ufId) || !stmt->bindUInt64(1, uid)) {
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

    fillUserFileRow(*stmt, out);
    return ErrorCode::Ok;
}

ErrorCode FileDao::listByParent(const FileListQuery& query, FileListPage* page) {
    if (page == NULL || query.uid == 0 || query.limit == 0 ||
        query.limit > kMaxListLimit) {
        return ErrorCode::InvalidArgument;
    }

    page->rows.clear();
    page->hasMore = false;
    page->nextKey.clear();
    page->nextId = 0;

    const bool firstPage = query.cursorKey.empty() && query.cursorId == 0;
    if (!firstPage && (query.cursorKey.empty() || query.cursorId == 0)) {
        return ErrorCode::InvalidArgument;
    }

    // keyset 分页：游标是索引 seek，(key, uf_id) 保证同微秒多行也稳定。
    // LEFT JOIN t_file 回填 size/md5；目录 f_id 为 NULL，COALESCE 后是 0 / 空串。
    // keyset 谓词里的 uf_name / uf_created_at / uf_updated_at / uf_id 只存在于
    // t_user_file，保持不限定表名，避免 filesort 之外的语义变化。
    static const char* kSelectPrefix =
        "SELECT uf.uf_id, uf.u_id, uf.uf_parent_id, uf.f_id, uf.uf_name, "
        "uf.uf_is_dir, uf.uf_deleted_at, uf.uf_created_at, uf.uf_updated_at, "
        "COALESCE(f.f_size, 0), COALESCE(f.f_md5, '') "
        "FROM t_user_file uf LEFT JOIN t_file f ON f.f_id = uf.f_id "
        "WHERE uf.u_id = ? AND uf.uf_parent_id = ? AND uf.uf_deleted_at = ? ";

    std::string sql = kSelectPrefix;
    switch (query.sort) {
        case FileListSort::NameAsc:
            if (!firstPage) {
                sql += "AND (uf_name, uf_id) > (?, ?) ";
            }
            sql += "ORDER BY uf_name, uf_id LIMIT ?";
            break;
        case FileListSort::CreatedDesc:
            if (!firstPage) {
                sql += "AND (uf_created_at, uf_id) < (?, ?) ";
            }
            sql += "ORDER BY uf_created_at DESC, uf_id DESC LIMIT ?";
            break;
        case FileListSort::UpdatedDesc:
            if (!firstPage) {
                sql += "AND (uf_updated_at, uf_id) < (?, ?) ";
            }
            sql += "ORDER BY uf_updated_at DESC, uf_id DESC LIMIT ?";
            break;
        default:
            return ErrorCode::InvalidArgument;
    }

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(sql);
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindUInt64(0, query.uid) ||
        !stmt->bindUInt64(1, query.parentId) ||
        !stmt->bindString(2, kActiveDeletedAt)) {
        return stmt->lastError();
    }

    std::size_t limitIndex = 3;
    if (!firstPage) {
        if (!stmt->bindString(3, query.cursorKey) ||
            !stmt->bindUInt64(4, query.cursorId)) {
            return stmt->lastError();
        }
        limitIndex = 5;
    }
    if (!stmt->bindUInt64(limitIndex,
                          static_cast<std::uint64_t>(query.limit + 1))) {
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
        UserFileRow row;
        fillUserFileRow(*stmt, &row);
        row.size = stmt->getUInt64(9);
        row.md5 = stmt->getString(10);
        page->rows.push_back(row);
        if (page->rows.size() > query.limit) {
            // 多取的一行只用于 has_more，不返回给客户端。
            page->rows.pop_back();
            page->hasMore = true;
            break;
        }
    }

    if (!page->rows.empty()) {
        page->nextKey = (query.sort == FileListSort::NameAsc)
                            ? page->rows.back().name
                            : (query.sort == FileListSort::CreatedDesc
                                   ? page->rows.back().createdAt
                                   : page->rows.back().updatedAt);
        page->nextId = page->rows.back().ufId;
    }
    return ErrorCode::Ok;
}

ErrorCode FileDao::countByParent(std::uint64_t uid,
                                 std::uint64_t parentId,
                                 std::uint64_t* count) {
    if (count == NULL || uid == 0) {
        return ErrorCode::InvalidArgument;
    }
    *count = 0;

    // 200ms 提示 + LIMIT 10001：大目录只统计到“超过 1 万”，不扫全表。
    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "SELECT /*+ MAX_EXECUTION_TIME(200) */ COUNT(*) FROM ("
        "SELECT 1 FROM t_user_file "
        "WHERE u_id = ? AND uf_parent_id = ? AND uf_deleted_at = ? LIMIT 10001"
        ") t");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindUInt64(0, uid) || !stmt->bindUInt64(1, parentId) ||
        !stmt->bindString(2, kActiveDeletedAt)) {
        return stmt->lastError();
    }
    if (!stmt->execute()) {
        if (isExecutionTimeout(stmt->lastErrorMessage())) {
            // 超时只降级为“未知”，列表本身照常返回。
            *count = kUnknownCount;
            return ErrorCode::Ok;
        }
        return stmt->lastError();
    }

    const PreparedStatement::FetchStatus status = stmt->fetchRow();
    if (status != PreparedStatement::FetchStatus::Row) {
        return status == PreparedStatement::FetchStatus::End
                   ? ErrorCode::DbQueryFailed
                   : stmt->lastError();
    }
    *count = stmt->getUInt64(0);
    return ErrorCode::Ok;
}

ErrorCode FileDao::softDelete(std::uint64_t uid,
                              std::uint64_t ufId,
                              bool* alreadyDeleted) {
    if (alreadyDeleted == NULL || uid == 0 || ufId == 0) {
        return ErrorCode::InvalidArgument;
    }
    *alreadyDeleted = false;

    Transaction tx(m_conn);
    if (!tx.valid()) {
        return m_conn.lastError();
    }

    std::uint64_t fid = 0;
    bool isDir = false;
    std::string deletedAt;
    const ErrorCode locked =
        lockUserFile(m_conn, uid, ufId, &fid, &isDir, &deletedAt, NULL);
    if (locked != ErrorCode::Ok) {
        return locked;
    }
    if (deletedAt != kActiveDeletedAt) {
        *alreadyDeleted = true;
        if (!tx.commit()) {
            return tx.lastError();
        }
        return ErrorCode::Ok;
    }

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "UPDATE t_user_file SET uf_deleted_at = NOW(6) WHERE uf_id = ? AND u_id = ?");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindUInt64(0, ufId) || !stmt->bindUInt64(1, uid)) {
        return stmt->lastError();
    }
    if (!stmt->execute()) {
        return stmt->lastError();
    }

    if (!isDir && fid != 0) {
        const ErrorCode decremented = decrementBlobRef(m_conn, fid, NULL);
        if (decremented != ErrorCode::Ok) {
            return decremented;
        }
    }

    if (!tx.commit()) {
        return tx.lastError();
    }
    return ErrorCode::Ok;
}

ErrorCode FileDao::restore(std::uint64_t uid,
                           std::uint64_t ufId,
                           std::uint64_t parentId,
                           const std::string& name,
                           RestoreOutcome* out) {
    if (out == NULL || uid == 0 || ufId == 0 || name.empty() || name.size() > 255) {
        return ErrorCode::InvalidArgument;
    }
    *out = RestoreOutcome();

    Transaction tx(m_conn);
    if (!tx.valid()) {
        return m_conn.lastError();
    }

    std::uint64_t fid = 0;
    bool isDir = false;
    std::string deletedAt;
    std::string currentName;
    const ErrorCode locked = lockUserFile(m_conn, uid, ufId, &fid, &isDir,
                                          &deletedAt, &currentName);
    if (locked != ErrorCode::Ok) {
        return locked;
    }

    out->fid = fid;
    if (deletedAt == kActiveDeletedAt) {
        out->alreadyRestored = true;
        out->name = currentName;
        if (!tx.commit()) {
            return tx.lastError();
        }
        return ErrorCode::Ok;
    }

    std::string restoredName;
    bool restored = false;
    for (int attempt = 1; attempt <= kMaxRenameAttempt; ++attempt) {
        const std::string candidate = candidateName(name, attempt);

        std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
            "UPDATE t_user_file SET uf_deleted_at = ?, uf_parent_id = ?, "
            "uf_name = ? WHERE uf_id = ? AND u_id = ?");
        if (!stmt) {
            return m_conn.lastError();
        }
        if (!stmt->bindString(0, kActiveDeletedAt) ||
            !stmt->bindUInt64(1, parentId) || !stmt->bindString(2, candidate) ||
            !stmt->bindUInt64(3, ufId) || !stmt->bindUInt64(4, uid)) {
            return stmt->lastError();
        }

        if (stmt->execute()) {
            restoredName = candidate;
            restored = true;
            break;
        }
        if (!isDuplicateEntry(stmt->lastErrorMessage())) {
            return stmt->lastError();
        }
    }
    if (!restored) {
        return ErrorCode::AlreadyExists;
    }

    if (!isDir && fid != 0) {
        const ErrorCode incremented = incrementBlobRef(m_conn, fid);
        if (incremented != ErrorCode::Ok) {
            return incremented;
        }
    }

    if (!tx.commit()) {
        return tx.lastError();
    }
    out->name = restoredName;
    return ErrorCode::Ok;
}

ErrorCode FileDao::purgeNode(std::uint64_t uid,
                             std::uint64_t ufId,
                             bool* deleted) {
    if (deleted == NULL || uid == 0 || ufId == 0) {
        return ErrorCode::InvalidArgument;
    }
    *deleted = false;

    Transaction tx(m_conn);
    if (!tx.valid()) {
        return m_conn.lastError();
    }

    std::uint64_t fid = 0;
    bool isDir = false;
    std::string deletedAt;
    const ErrorCode locked =
        lockUserFile(m_conn, uid, ufId, &fid, &isDir, &deletedAt, NULL);
    if (locked != ErrorCode::Ok) {
        return locked;
    }

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "DELETE FROM t_user_file WHERE uf_id = ? AND u_id = ?");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindUInt64(0, ufId) || !stmt->bindUInt64(1, uid)) {
        return stmt->lastError();
    }
    if (!stmt->execute()) {
        return stmt->lastError();
    }

    // 软删时已减过计数；只有从未软删过的活跃文件节点需要在此补减一次。
    if (!isDir && fid != 0 && deletedAt == kActiveDeletedAt) {
        const ErrorCode decremented = decrementBlobRef(m_conn, fid, NULL);
        if (decremented != ErrorCode::Ok) {
            return decremented;
        }
    }

    if (!tx.commit()) {
        return tx.lastError();
    }
    *deleted = true;
    return ErrorCode::Ok;
}

ErrorCode FileDao::listRecycle(const RecycleListQuery& query,
                               RecycleListPage* page) {
    if (page == NULL || query.uid == 0 || query.limit == 0 ||
        query.limit > kMaxListLimit) {
        return ErrorCode::InvalidArgument;
    }

    page->rows.clear();
    page->hasMore = false;
    page->nextDeletedMs = 0;
    page->nextId = 0;

    const bool firstPage = query.cursorDeletedMs == 0 && query.cursorId == 0;
    if (!firstPage && (query.cursorDeletedMs == 0 || query.cursorId == 0)) {
        return ErrorCode::InvalidArgument;
    }

    // 用 UNIX_TIMESTAMP 把 DATETIME(6) 折算成毫秒，排序键与 wire 游标同口径：
    // 同毫秒多行由 uf_id 兜底，(ms, id) 是精确 keyset，不丢行不重复。
    const char* kMsExpr =
        "CAST(UNIX_TIMESTAMP(uf.uf_deleted_at)*1000 AS UNSIGNED)";
    std::string sql =
        "SELECT uf.uf_id, uf.f_id, uf.uf_name, uf.uf_is_dir, uf.uf_parent_id, ";
    sql += kMsExpr;
    sql += " AS deleted_ms, COALESCE(f.f_size, 0) AS size "
           "FROM t_user_file uf LEFT JOIN t_file f ON f.f_id = uf.f_id "
           "WHERE uf.u_id = ? AND uf.uf_deleted_at > ? ";
    if (!firstPage) {
        sql += "AND (";
        sql += kMsExpr;
        sql += ", uf.uf_id) < (?, ?) ";
    }
    sql += "ORDER BY ";
    sql += kMsExpr;
    sql += " DESC, uf.uf_id DESC LIMIT ?";

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(sql);
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindUInt64(0, query.uid) || !stmt->bindString(1, kActiveDeletedAt)) {
        return stmt->lastError();
    }

    std::size_t limitIndex = 2;
    if (!firstPage) {
        if (!stmt->bindUInt64(2, query.cursorDeletedMs) ||
            !stmt->bindUInt64(3, query.cursorId)) {
            return stmt->lastError();
        }
        limitIndex = 4;
    }
    if (!stmt->bindUInt64(limitIndex,
                          static_cast<std::uint64_t>(query.limit + 1))) {
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
        RecycleNode row;
        row.ufId = stmt->getUInt64(0);
        row.fid = stmt->isNull(1) ? 0 : stmt->getUInt64(1);
        row.name = stmt->getString(2);
        row.isDir = stmt->getUInt64(3) != 0;
        row.parentId = stmt->getUInt64(4);
        row.deletedAtMs = stmt->getUInt64(5);
        row.size = stmt->getUInt64(6);
        page->rows.push_back(row);
        if (page->rows.size() > query.limit) {
            page->rows.pop_back();
            page->hasMore = true;
            break;
        }
    }

    if (!page->rows.empty()) {
        page->nextDeletedMs = page->rows.back().deletedAtMs;
        page->nextId = page->rows.back().ufId;
    }
    return ErrorCode::Ok;
}

ErrorCode FileDao::listSubtree(std::uint64_t uid,
                               std::uint64_t rootUfId,
                               std::size_t maxNodes,
                               std::vector<UserFileRow>* out) {
    if (out == NULL || uid == 0 || rootUfId == 0 || maxNodes == 0) {
        return ErrorCode::InvalidArgument;
    }
    out->clear();

    UserFileRow root;
    const ErrorCode rootRc = findUserFile(uid, rootUfId, &root);
    if (rootRc != ErrorCode::Ok) {
        return rootRc;
    }
    out->push_back(root);

    // BFS：逐层列子节点（不过滤 deleted_at，软删后代也要一并物理删除）。
    std::vector<std::uint64_t> queue;
    queue.push_back(rootUfId);
    std::size_t head = 0;
    while (head < queue.size()) {
        const std::uint64_t parentId = queue[head++];

        std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
            "SELECT uf_id, u_id, uf_parent_id, f_id, uf_name, uf_is_dir, "
            "uf_deleted_at, uf_created_at, uf_updated_at FROM t_user_file "
            "WHERE u_id = ? AND uf_parent_id = ? ORDER BY uf_id");
        if (!stmt) {
            return m_conn.lastError();
        }
        if (!stmt->bindUInt64(0, uid) || !stmt->bindUInt64(1, parentId)) {
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
            UserFileRow child;
            fillUserFileRow(*stmt, &child);
            out->push_back(child);
            if (out->size() > maxNodes) {
                return ErrorCode::TooManyNodes;
            }
            if (child.isDir) {
                queue.push_back(child.ufId);
            }
        }
    }
    return ErrorCode::Ok;
}

ErrorCode FileDao::purgeNodesInTx(std::uint64_t uid,
                                  const std::vector<std::uint64_t>& ufIds,
                                  PurgeBatchResult* out) {
    if (out == NULL || uid == 0) {
        return ErrorCode::InvalidArgument;
    }
    *out = PurgeBatchResult();

    // 调用方必须已 BEGIN；这里只做锁行 → DELETE → 补减计数。
    for (std::size_t i = 0; i < ufIds.size(); ++i) {
        std::uint64_t fid = 0;
        bool isDir = false;
        std::string deletedAt;
        const ErrorCode locked =
            lockUserFile(m_conn, uid, ufIds[i], &fid, &isDir, &deletedAt, NULL);
        if (locked == ErrorCode::NotFound) {
            continue;  // 已删（重跑幂等），跳过
        }
        if (locked != ErrorCode::Ok) {
            return locked;
        }

        std::unique_ptr<PreparedStatement> del = m_conn.prepare(
            "DELETE FROM t_user_file WHERE uf_id = ? AND u_id = ?");
        if (!del) {
            return m_conn.lastError();
        }
        if (!del->bindUInt64(0, ufIds[i]) || !del->bindUInt64(1, uid)) {
            return del->lastError();
        }
        if (!del->execute()) {
            return del->lastError();
        }

        // 从未软删过的活跃文件节点在此补减一次计数；已软删的不再减。
        if (!isDir && fid != 0 && deletedAt == kActiveDeletedAt) {
            bool orphaned = false;
            const ErrorCode decremented = decrementBlobRef(m_conn, fid, &orphaned);
            if (decremented != ErrorCode::Ok) {
                return decremented;
            }
            if (orphaned) {
                ++out->blobsOrphaned;
            }
        }

        if (isDir) {
            ++out->dirsDeleted;
        } else {
            ++out->filesDeleted;
        }
    }
    return ErrorCode::Ok;
}

}  // namespace db
}  // namespace cm
