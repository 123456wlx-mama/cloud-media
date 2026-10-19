#include "share_dao.h"

#include <cctype>
#include <memory>

#include "mysql_conn.h"
#include "prepared_statement.h"

namespace cm {
namespace db {
namespace {

// 去掉 0/O/1/I/L 的 31 字符表（06 §7.1）。
const char kShareAlphabet[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
const std::size_t kShareCodeLength = 6;
const std::size_t kMaxShareListLimit = 200;
const std::size_t kMaxSweepLimit = 1000;

const char kShareColumns[] =
    "s_id, s_code, s_user_id, s_uf_id, s_file_id, s_access_code, s_status, "
    "s_expire_at, s_created_at, s_updated_at";

bool isDuplicateEntry(const std::string& message) {
    return message.find("Duplicate entry") != std::string::npos ||
           message.find("ER_DUP_ENTRY") != std::string::npos;
}

bool isFourDigits(const std::string& value) {
    if (value.size() != 4) {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(value[i])) == 0) {
            return false;
        }
    }
    return true;
}

void fillShareRow(const PreparedStatement& stmt, ShareRow* row) {
    row->shareId = stmt.getUInt64(0);
    row->code = stmt.getString(1);
    row->userId = stmt.getUInt64(2);
    row->ufId = stmt.getUInt64(3);
    row->fileId = stmt.isNull(4) ? 0 : stmt.getUInt64(4);
    row->accessCode = stmt.isNull(5) ? std::string() : stmt.getString(5);
    row->status = static_cast<std::uint8_t>(stmt.getUInt64(6));
    row->expireAt = stmt.getString(7);
    row->createdAt = stmt.getString(8);
    row->updatedAt = stmt.getString(9);
}

}  // namespace

ShareRow::ShareRow()
    : shareId(0), userId(0), ufId(0), fileId(0), status(0) {
}

NewShare::NewShare()
    : userId(0), ufId(0), fileId(0), isDir(false), expirePolicy(2) {
}

ShareListQuery::ShareListQuery() : userId(0), limit(0), cursorId(0) {
}

ShareListPage::ShareListPage() : hasMore(false), nextId(0) {
}

ShareDao::ShareDao(MysqlConn& conn) : m_conn(conn) {
}

const char* ShareDao::codeAlphabet() {
    return kShareAlphabet;
}

bool ShareDao::isValidCode(const std::string& code) {
    if (code.size() != kShareCodeLength) {
        return false;
    }
    for (std::size_t i = 0; i < code.size(); ++i) {
        bool found = false;
        for (std::size_t j = 0; kShareAlphabet[j] != '\0'; ++j) {
            if (kShareAlphabet[j] == code[i]) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

ErrorCode ShareDao::insert(const NewShare& share, std::uint64_t* shareId) {
    if (shareId == NULL || !isValidCode(share.code) || share.userId == 0 ||
        share.ufId == 0 || share.expirePolicy > 2) {
        return ErrorCode::InvalidArgument;
    }
    if (share.isDir ? share.fileId != 0 : share.fileId == 0) {
        return ErrorCode::InvalidArgument;
    }
    if (!share.accessCode.empty() && !isFourDigits(share.accessCode)) {
        return ErrorCode::InvalidArgument;
    }
    *shareId = 0;

    // 有效期在 SQL 内按 NOW(6) 计算：0=永久哨兵，1=+1d，2=+7d。
    std::uint64_t days = 0;
    if (share.expirePolicy == 1) {
        days = 1;
    } else if (share.expirePolicy == 2) {
        days = 7;
    }

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "INSERT INTO t_share (s_code, s_user_id, s_uf_id, s_file_id, "
        "s_access_code, s_expire_at) "
        "VALUES (?, ?, ?, ?, ?, IF(? = 0, '9999-12-31 23:59:59.999999', "
        "DATE_ADD(NOW(6), INTERVAL ? DAY)))");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindString(0, share.code) || !stmt->bindUInt64(1, share.userId) ||
        !stmt->bindUInt64(2, share.ufId)) {
        return stmt->lastError();
    }
    if (share.isDir) {
        if (!stmt->bindNull(3)) {
            return stmt->lastError();
        }
    } else if (!stmt->bindUInt64(3, share.fileId)) {
        return stmt->lastError();
    }
    if (share.accessCode.empty()) {
        if (!stmt->bindNull(4)) {
            return stmt->lastError();
        }
    } else if (!stmt->bindString(4, share.accessCode)) {
        return stmt->lastError();
    }
    if (!stmt->bindUInt64(5, share.expirePolicy) || !stmt->bindUInt64(6, days)) {
        return stmt->lastError();
    }

    if (!stmt->execute()) {
        // 1062 原样抛给服务层重试，DAO 不换码、不吞错。
        if (isDuplicateEntry(stmt->lastErrorMessage())) {
            return ErrorCode::AlreadyExists;
        }
        return stmt->lastError();
    }

    *shareId = stmt->insertId();
    return *shareId == 0 ? ErrorCode::DbQueryFailed : ErrorCode::Ok;
}

ErrorCode ShareDao::findByCode(const std::string& code, ShareRow* out) {
    if (out == NULL || !isValidCode(code)) {
        return ErrorCode::InvalidArgument;
    }

    std::string sql = "SELECT ";
    sql += kShareColumns;
    sql += " FROM t_share WHERE s_code = ?";

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(sql);
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindString(0, code)) {
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
    fillShareRow(*stmt, out);
    return ErrorCode::Ok;
}

ErrorCode ShareDao::listByUser(const ShareListQuery& query, ShareListPage* page) {
    if (page == NULL || query.userId == 0 || query.limit == 0 ||
        query.limit > kMaxShareListLimit) {
        return ErrorCode::InvalidArgument;
    }

    page->rows.clear();
    page->hasMore = false;
    page->nextCreatedAt.clear();
    page->nextId = 0;

    const bool firstPage = query.cursorCreatedAt.empty() && query.cursorId == 0;
    if (!firstPage && (query.cursorCreatedAt.empty() || query.cursorId == 0)) {
        return ErrorCode::InvalidArgument;
    }

    std::string sql = "SELECT ";
    sql += kShareColumns;
    sql +=
        " FROM t_share WHERE s_user_id = ? AND s_status = 0 "
        "AND s_expire_at > NOW(6) ";
    if (!firstPage) {
        sql += "AND (s_created_at, s_id) < (?, ?) ";
    }
    sql += "ORDER BY s_created_at DESC, s_id DESC LIMIT ?";

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(sql);
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindUInt64(0, query.userId)) {
        return stmt->lastError();
    }

    std::size_t limitIndex = 1;
    if (!firstPage) {
        if (!stmt->bindString(1, query.cursorCreatedAt) ||
            !stmt->bindUInt64(2, query.cursorId)) {
            return stmt->lastError();
        }
        limitIndex = 3;
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
        ShareRow row;
        fillShareRow(*stmt, &row);
        page->rows.push_back(row);
        if (page->rows.size() > query.limit) {
            page->rows.pop_back();
            page->hasMore = true;
            break;
        }
    }

    if (!page->rows.empty()) {
        page->nextCreatedAt = page->rows.back().createdAt;
        page->nextId = page->rows.back().shareId;
    }
    return ErrorCode::Ok;
}

ErrorCode ShareDao::cancel(std::uint64_t userId,
                           const std::string& code,
                           bool* cancelled,
                           bool* already) {
    if (cancelled == NULL || already == NULL || userId == 0 ||
        !isValidCode(code)) {
        return ErrorCode::InvalidArgument;
    }
    *cancelled = false;
    *already = false;

    Transaction tx(m_conn);
    if (!tx.valid()) {
        return m_conn.lastError();
    }

    std::unique_ptr<PreparedStatement> update = m_conn.prepare(
        "UPDATE t_share SET s_status = 1 "
        "WHERE s_code = ? AND s_user_id = ? AND s_status = 0");
    if (!update) {
        return m_conn.lastError();
    }
    if (!update->bindString(0, code) || !update->bindUInt64(1, userId)) {
        return update->lastError();
    }
    if (!update->execute()) {
        return update->lastError();
    }

    if (update->affectedRows() > 0) {
        if (!tx.commit()) {
            return tx.lastError();
        }
        *cancelled = true;
        return ErrorCode::Ok;
    }
    update.reset();

    // 已取消（或已被并发请求取消）：读一次区分 NotFound 与 already。
    std::unique_ptr<PreparedStatement> select = m_conn.prepare(
        "SELECT s_status FROM t_share WHERE s_code = ? AND s_user_id = ?");
    if (!select) {
        return m_conn.lastError();
    }
    if (!select->bindString(0, code) || !select->bindUInt64(1, userId)) {
        return select->lastError();
    }
    if (!select->execute()) {
        return select->lastError();
    }

    const PreparedStatement::FetchStatus status = select->fetchRow();
    if (status == PreparedStatement::FetchStatus::End) {
        return ErrorCode::NotFound;
    }
    if (status == PreparedStatement::FetchStatus::Error) {
        return select->lastError();
    }

    *already = true;
    if (!tx.commit()) {
        return tx.lastError();
    }
    return ErrorCode::Ok;
}

ErrorCode ShareDao::listActiveByUfId(std::uint64_t ufId,
                                    std::vector<std::string>* codes) {
    if (codes == NULL || ufId == 0) {
        return ErrorCode::InvalidArgument;
    }
    codes->clear();

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "SELECT s_code FROM t_share WHERE s_uf_id = ? AND s_status = 0 "
        "ORDER BY s_id ASC");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindUInt64(0, ufId)) {
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
        codes->push_back(stmt->getString(0));
    }
    return ErrorCode::Ok;
}

ErrorCode ShareDao::sweepExpired(std::size_t limit, std::uint64_t* affected) {
    if (affected == NULL || limit == 0 || limit > kMaxSweepLimit) {
        return ErrorCode::InvalidArgument;
    }
    *affected = 0;

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "UPDATE t_share SET s_status = 1 "
        "WHERE s_status = 0 AND s_expire_at < NOW(6) LIMIT ?");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindUInt64(0, static_cast<std::uint64_t>(limit))) {
        return stmt->lastError();
    }
    if (!stmt->execute()) {
        return stmt->lastError();
    }

    *affected = stmt->affectedRows();
    return ErrorCode::Ok;
}

}  // namespace db
}  // namespace cm
