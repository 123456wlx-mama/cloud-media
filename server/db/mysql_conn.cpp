#include "mysql_conn.h"

#include <utility>

#include "mysql_pool.h"
#include "prepared_statement.h"

namespace cm {
namespace db {

MysqlConn::MysqlConn(const std::shared_ptr<detail::PoolCore>& core,
                     MysqlHandle handle,
                     std::chrono::steady_clock::time_point createdAt,
                     std::chrono::milliseconds slowQueryThreshold)
    : m_core(core),
      m_handle(std::move(handle)),
      m_createdAt(createdAt),
      m_slowQueryThreshold(slowQueryThreshold),
      m_broken(false),
      m_inTx(false),
      m_lastError(ErrorCode::Ok) {
}

MysqlConn::~MysqlConn() {
    if (!m_core) {
        return;
    }
    m_core->giveBack(std::move(m_handle), m_createdAt,
                     std::chrono::steady_clock::now(),
                     m_broken || m_inTx);
}

bool MysqlConn::valid() const {
    return m_handle.get() != NULL && !m_broken;
}

bool MysqlConn::ping() {
    if (!valid()) {
        setError(ErrorCode::DbUnavailable, "connection is closed");
        return false;
    }
    if (mysql_ping(m_handle.get()) != 0) {
        setError(ErrorCode::DbUnavailable, mysql_error(m_handle.get()));
        markBroken();
        return false;
    }
    return true;
}

bool MysqlConn::inTransaction() const {
    return m_inTx;
}

std::uint64_t MysqlConn::id() const {
    if (m_handle.get() == NULL) {
        return 0;
    }
    return static_cast<std::uint64_t>(mysql_thread_id(m_handle.get()));
}

std::unique_ptr<PreparedStatement> MysqlConn::prepare(const std::string& sql) {
    m_lastError = ErrorCode::Ok;
    m_lastErrorMsg.clear();
    if (!valid()) {
        setError(ErrorCode::DbUnavailable, "connection is closed");
        return std::unique_ptr<PreparedStatement>();
    }

    MYSQL_STMT* stmt = mysql_stmt_init(m_handle.get());
    if (stmt == NULL) {
        setError(ErrorCode::DbQueryFailed, mysql_error(m_handle.get()));
        return std::unique_ptr<PreparedStatement>();
    }
    if (mysql_stmt_prepare(stmt, sql.c_str(),
                           static_cast<unsigned long>(sql.size())) != 0) {
        const std::string message = mysql_stmt_error(stmt);
        const unsigned int code = mysql_stmt_errno(stmt);
        mysql_stmt_close(stmt);
        setMysqlError(code, message);
        return std::unique_ptr<PreparedStatement>();
    }

    return std::unique_ptr<PreparedStatement>(
        new PreparedStatement(m_core.get(), this, m_handle.get(), stmt, sql,
                              m_slowQueryThreshold, m_inTx));
}

bool MysqlConn::executeDdl(const std::string& sql) {
    m_lastError = ErrorCode::Ok;
    m_lastErrorMsg.clear();
    if (!valid()) {
        setError(ErrorCode::DbUnavailable, "connection is closed");
        return false;
    }
    if (mysql_query(m_handle.get(), sql.c_str()) != 0) {
        setMysqlError(mysql_errno(m_handle.get()), mysql_error(m_handle.get()));
        return false;
    }
    return true;
}

bool MysqlConn::begin() {
    m_lastError = ErrorCode::Ok;
    m_lastErrorMsg.clear();
    if (!valid()) {
        setError(ErrorCode::DbUnavailable, "connection is closed");
        return false;
    }
    if (m_inTx) {
        setError(ErrorCode::InvalidArgument, "nested transaction is not allowed");
        return false;
    }
    if (mysql_query(m_handle.get(), "START TRANSACTION") != 0) {
        setError(ErrorCode::DbQueryFailed, mysql_error(m_handle.get()));
        markBroken();
        return false;
    }
    m_inTx = true;
    return true;
}

bool MysqlConn::commit() {
    m_lastError = ErrorCode::Ok;
    m_lastErrorMsg.clear();
    if (!valid() || !m_inTx) {
        setError(ErrorCode::InvalidArgument, "no active transaction");
        return false;
    }
    if (mysql_commit(m_handle.get()) != 0) {
        setError(ErrorCode::DbQueryFailed, mysql_error(m_handle.get()));
        m_inTx = false;
        markBroken();
        return false;
    }
    m_inTx = false;
    return true;
}

bool MysqlConn::rollback() {
    m_lastError = ErrorCode::Ok;
    m_lastErrorMsg.clear();
    if (!valid() || !m_inTx) {
        setError(ErrorCode::InvalidArgument, "no active transaction");
        return false;
    }
    if (mysql_rollback(m_handle.get()) != 0) {
        setError(ErrorCode::DbQueryFailed, mysql_error(m_handle.get()));
        m_inTx = false;
        markBroken();
        return false;
    }
    m_inTx = false;
    return true;
}

void MysqlConn::setError(ErrorCode code, const std::string& message) {
    m_lastError = code;
    m_lastErrorMsg = message;
}

void MysqlConn::setMysqlError(unsigned int code, const std::string& message) {
    const ErrorCode mapped = classifyMysqlError(code);
    setError(mapped, message);
    if (mapped == ErrorCode::DbUnavailable) {
        markBroken();
    }
}

void MysqlConn::markBroken() {
    m_broken = true;
}

Transaction::Transaction(MysqlConn& conn)
    : m_conn(conn), m_valid(false), m_finished(false) {
    m_valid = m_conn.begin();
}

Transaction::~Transaction() {
    if (m_valid && !m_finished) {
        m_conn.rollback();
    }
}

bool Transaction::commit() {
    if (!m_valid || m_finished) {
        return false;
    }
    m_finished = true;
    return m_conn.commit();
}

bool Transaction::rollback() {
    if (!m_valid || m_finished) {
        return false;
    }
    m_finished = true;
    return m_conn.rollback();
}

}  // namespace db
}  // namespace cm
