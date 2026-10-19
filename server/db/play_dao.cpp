#include "play_dao.h"

#include <memory>

#include "mysql_conn.h"
#include "prepared_statement.h"

namespace cm {
namespace db {
namespace {

const std::size_t kMaxRecentLimit = 200;

const char kPlayColumns[] =
    "p_id, p_user_id, p_file_id, p_position_ms, p_duration_ms, p_finished, "
    "p_updated_at";

void fillPlayRow(const PreparedStatement& stmt, PlayRow* row) {
    row->playId = stmt.getUInt64(0);
    row->userId = stmt.getUInt64(1);
    row->fileId = stmt.getUInt64(2);
    row->positionMs = stmt.getUInt64(3);
    row->durationMs = stmt.getUInt64(4);
    row->finished = stmt.getUInt64(5) != 0;
    row->updatedAt = stmt.getString(6);
}

}  // namespace

PlayRow::PlayRow()
    : playId(0), userId(0), fileId(0), positionMs(0), durationMs(0), finished(false) {
}

PlayDao::PlayDao(MysqlConn& conn) : m_conn(conn) {
}

ErrorCode PlayDao::upsertLww(std::uint64_t userId,
                             std::uint64_t fileId,
                             std::uint64_t positionMs,
                             std::uint64_t durationMs,
                             bool finished) {
    if (userId == 0 || fileId == 0) {
        return ErrorCode::InvalidArgument;
    }
    if (durationMs > 0 && positionMs > durationMs) {
        return ErrorCode::InvalidArgument;
    }

    // LWW：直接覆盖，允许回退；不用 MAX/IF 保留旧值（06 §4.19）。
    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "INSERT INTO t_play_record (p_user_id, p_file_id, p_position_ms, "
        "p_duration_ms, p_finished) VALUES (?, ?, ?, ?, ?) "
        "ON DUPLICATE KEY UPDATE p_position_ms = VALUES(p_position_ms), "
        "p_duration_ms = VALUES(p_duration_ms), "
        "p_finished = VALUES(p_finished), p_updated_at = NOW(6)");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindUInt64(0, userId) || !stmt->bindUInt64(1, fileId) ||
        !stmt->bindUInt64(2, positionMs) || !stmt->bindUInt64(3, durationMs) ||
        !stmt->bindUInt64(4, static_cast<std::uint64_t>(finished ? 1 : 0))) {
        return stmt->lastError();
    }
    if (!stmt->execute()) {
        return stmt->lastError();
    }
    return ErrorCode::Ok;
}

ErrorCode PlayDao::findByUserFile(std::uint64_t userId,
                                  std::uint64_t fileId,
                                  PlayRow* out) {
    if (out == NULL || userId == 0 || fileId == 0) {
        return ErrorCode::InvalidArgument;
    }

    std::string sql = "SELECT ";
    sql += kPlayColumns;
    sql += " FROM t_play_record WHERE p_user_id = ? AND p_file_id = ?";

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(sql);
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindUInt64(0, userId) || !stmt->bindUInt64(1, fileId)) {
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
    fillPlayRow(*stmt, out);
    return ErrorCode::Ok;
}

ErrorCode PlayDao::listRecent(std::uint64_t userId,
                              std::size_t limit,
                              std::vector<PlayRow>* out) {
    if (out == NULL || userId == 0 || limit == 0 || limit > kMaxRecentLimit) {
        return ErrorCode::InvalidArgument;
    }
    out->clear();

    // p_id 做 tie-break：同一微秒更新的多行也能稳定排序。
    std::string sql = "SELECT ";
    sql += kPlayColumns;
    sql +=
        " FROM t_play_record WHERE p_user_id = ? "
        "ORDER BY p_updated_at DESC, p_id DESC LIMIT ?";

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(sql);
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindUInt64(0, userId) ||
        !stmt->bindUInt64(1, static_cast<std::uint64_t>(limit))) {
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
        PlayRow row;
        fillPlayRow(*stmt, &row);
        out->push_back(row);
    }
    return ErrorCode::Ok;
}

}  // namespace db
}  // namespace cm
