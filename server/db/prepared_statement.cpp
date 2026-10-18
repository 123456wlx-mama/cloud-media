#include "prepared_statement.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "cm/log/logger.h"
#include "mysql_pool.h"

namespace cm {
namespace db {
namespace {

std::string truncateSql(const std::string& sql) {
    const std::size_t kMax = 512;
    if (sql.size() <= kMax) {
        return sql;
    }
    return sql.substr(0, kMax) + "...";
}

bool isIdentifier(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(text[0]);
    if (!(std::isalpha(first) || first == '_')) {
        return false;
    }
    for (std::size_t i = 1; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (!(std::isalnum(c) || c == '_')) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool isSlowQuery(std::chrono::microseconds elapsed,
                 std::chrono::milliseconds threshold) {
    return elapsed >= std::chrono::duration_cast<std::chrono::microseconds>(threshold);
}

std::string formatSlowQuery(const SlowQueryInfo& info) {
    std::ostringstream out;
    out << "slow_query sql=" << truncateSql(info.sql)
        << " cost_ms="
        << std::chrono::duration_cast<std::chrono::milliseconds>(info.elapsed).count()
        << " rows=" << info.rows
        << " in_tx=" << (info.inTransaction ? 1 : 0);
    return out.str();
}

std::string PreparedStatement::placeholders(std::size_t count) {
    if (count == 0) {
        return std::string();
    }
    std::string result;
    result.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) {
            result.push_back(',');
        }
        result.push_back('?');
    }
    return result;
}

std::string PreparedStatement::buildInsert(
    const std::string& table, const std::vector<std::string>& columns) {
    if (!isIdentifier(table) || columns.empty()) {
        return std::string();
    }
    std::string columnList;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (!isIdentifier(columns[i])) {
            return std::string();
        }
        if (i != 0) {
            columnList.push_back(',');
        }
        columnList += columns[i];
    }
    return "INSERT INTO " + table + " (" + columnList + ") VALUES (" +
           placeholders(columns.size()) + ")";
}

PreparedStatement::PreparedStatement(detail::PoolCore* core,
                                     MysqlConn* conn,
                                     MYSQL* mysql,
                                     MYSQL_STMT* stmt,
                                     const std::string& sql,
                                     std::chrono::milliseconds slowThreshold,
                                     bool inTransaction)
    : m_core(core),
      m_conn(conn),
      m_mysql(mysql),
      m_stmt(stmt),
      m_meta(NULL),
      m_sql(sql),
      m_paramCount(0),
      m_fieldCount(0),
      m_rows(0),
      m_hasResult(false),
      m_timing(false),
      m_reported(false),
      m_slowThreshold(slowThreshold),
      m_inTransaction(inTransaction),
      m_lastError(ErrorCode::Ok) {
    if (m_stmt != NULL) {
        m_paramCount = static_cast<std::size_t>(mysql_stmt_param_count(m_stmt));
    }
    m_paramBinds.resize(m_paramCount);
    for (std::size_t i = 0; i < m_paramBinds.size(); ++i) {
        std::memset(&m_paramBinds[i], 0, sizeof(MYSQL_BIND));
    }
    m_paramLengths.assign(m_paramCount, 0);
    m_paramNulls.assign(m_paramCount, 0);
    m_intValues.assign(m_paramCount, 0);
    m_uintValues.assign(m_paramCount, 0);
}

PreparedStatement::~PreparedStatement() {
    reportSlowIfNeeded();
    if (m_meta != NULL) {
        mysql_free_result(m_meta);
        m_meta = NULL;
    }
    if (m_stmt != NULL) {
        mysql_stmt_close(m_stmt);
        m_stmt = NULL;
    }
}

void PreparedStatement::setError(ErrorCode code, const std::string& message) {
    m_lastError = code;
    m_lastErrorMsg = message;
}

bool PreparedStatement::bindInt64(std::size_t index, std::int64_t value) {
    if (index >= m_paramCount) {
        setError(ErrorCode::InvalidArgument, "bind index out of range");
        return false;
    }
    m_intValues[index] = value;
    MYSQL_BIND& bind = m_paramBinds[index];
    std::memset(&bind, 0, sizeof(MYSQL_BIND));
    bind.buffer_type = MYSQL_TYPE_LONGLONG;
    bind.buffer = &m_intValues[index];
    bind.buffer_length = sizeof(std::int64_t);
    bind.is_unsigned = 0;
    bind.is_null = &m_paramNulls[index];
    m_paramNulls[index] = 0;
    return true;
}

bool PreparedStatement::bindUInt64(std::size_t index, std::uint64_t value) {
    if (index >= m_paramCount) {
        setError(ErrorCode::InvalidArgument, "bind index out of range");
        return false;
    }
    m_uintValues[index] = value;
    MYSQL_BIND& bind = m_paramBinds[index];
    std::memset(&bind, 0, sizeof(MYSQL_BIND));
    bind.buffer_type = MYSQL_TYPE_LONGLONG;
    bind.buffer = &m_uintValues[index];
    bind.buffer_length = sizeof(std::uint64_t);
    bind.is_unsigned = 1;
    bind.is_null = &m_paramNulls[index];
    m_paramNulls[index] = 0;
    return true;
}

bool PreparedStatement::bindString(std::size_t index, const std::string& value) {
    if (index >= m_paramCount) {
        setError(ErrorCode::InvalidArgument, "bind index out of range");
        return false;
    }
    m_paramStrings.push_back(value);
    std::string& stored = m_paramStrings.back();
    m_paramLengths[index] = static_cast<unsigned long>(stored.size());
    m_paramNulls[index] = 0;

    MYSQL_BIND& bind = m_paramBinds[index];
    std::memset(&bind, 0, sizeof(MYSQL_BIND));
    bind.buffer_type = MYSQL_TYPE_STRING;
    bind.buffer = const_cast<char*>(stored.data());
    bind.buffer_length = static_cast<unsigned long>(stored.size());
    bind.length = &m_paramLengths[index];
    bind.is_null = &m_paramNulls[index];
    return true;
}

bool PreparedStatement::bindNull(std::size_t index) {
    if (index >= m_paramCount) {
        setError(ErrorCode::InvalidArgument, "bind index out of range");
        return false;
    }
    m_paramNulls[index] = 1;
    MYSQL_BIND& bind = m_paramBinds[index];
    std::memset(&bind, 0, sizeof(MYSQL_BIND));
    bind.buffer_type = MYSQL_TYPE_NULL;
    bind.buffer = NULL;
    bind.buffer_length = 0;
    bind.is_null = &m_paramNulls[index];
    return true;
}

bool PreparedStatement::execute() {
    m_lastError = ErrorCode::Ok;
    m_lastErrorMsg.clear();
    m_rows = 0;
    m_reported = false;
    if (m_meta != NULL) {
        mysql_free_result(m_meta);
        m_meta = NULL;
    }
    if (m_hasResult && m_stmt != NULL) {
        mysql_stmt_free_result(m_stmt);
    }
    m_hasResult = false;
    m_timing = true;
    m_startedAt = std::chrono::steady_clock::now();

    if (m_stmt == NULL) {
        m_timing = false;
        setError(ErrorCode::DbQueryFailed, "statement is closed");
        return false;
    }
    if (m_paramCount > 0 &&
        mysql_stmt_bind_param(m_stmt, &m_paramBinds[0]) != 0) {
        m_timing = false;
        setError(ErrorCode::DbQueryFailed, mysql_stmt_error(m_stmt));
        return false;
    }
    if (mysql_stmt_execute(m_stmt) != 0) {
        m_timing = false;
        const unsigned int code = mysql_stmt_errno(m_stmt);
        setError(ErrorCode::DbQueryFailed, mysql_stmt_error(m_stmt));
        if (m_conn != NULL && (code == 2003 || code == 2006 || code == 2013)) {
            m_conn->markBroken();
        }
        return false;
    }

    m_fieldCount = static_cast<std::size_t>(mysql_stmt_field_count(m_stmt));
    if (m_fieldCount == 0) {
        reportSlowIfNeeded();
        return true;
    }
    if (!prepareResult()) {
        reportSlowIfNeeded();
        return false;
    }
    m_hasResult = true;
    return true;
}

bool PreparedStatement::prepareResult() {
    m_meta = mysql_stmt_result_metadata(m_stmt);
    if (m_meta == NULL) {
        setError(ErrorCode::DbQueryFailed, mysql_stmt_error(m_stmt));
        return false;
    }
    m_fieldCount = static_cast<std::size_t>(mysql_num_fields(m_meta));
    MYSQL_FIELD* fields = mysql_fetch_fields(m_meta);
    if (fields == NULL || m_fieldCount == 0) {
        setError(ErrorCode::DbQueryFailed, "result metadata is empty");
        return false;
    }

    m_resultBinds.resize(m_fieldCount);
    m_resultBuffers.resize(m_fieldCount);
    m_resultLengths.assign(m_fieldCount, 0);
    m_resultNulls.assign(m_fieldCount, 0);
    m_resultErrors.assign(m_fieldCount, 0);

    const unsigned long kMinBuffer = 4096;
    const unsigned long kMaxBuffer = 1024UL * 1024UL;

    for (std::size_t i = 0; i < m_fieldCount; ++i) {
        unsigned long maxLength = fields[i].max_length;
        if (maxLength < kMinBuffer) {
            maxLength = kMinBuffer;
        }
        if (maxLength > kMaxBuffer) {
            maxLength = kMaxBuffer;
        }
        m_resultBuffers[i].resize(static_cast<std::size_t>(maxLength) + 1);

        MYSQL_BIND& bind = m_resultBinds[i];
        std::memset(&bind, 0, sizeof(MYSQL_BIND));
        bind.buffer_type = MYSQL_TYPE_STRING;
        bind.buffer = &m_resultBuffers[i][0];
        bind.buffer_length = maxLength;
        bind.length = &m_resultLengths[i];
        bind.is_null = &m_resultNulls[i];
        bind.error = &m_resultErrors[i];
    }

    if (mysql_stmt_bind_result(m_stmt, &m_resultBinds[0]) != 0) {
        setError(ErrorCode::DbQueryFailed, mysql_stmt_error(m_stmt));
        return false;
    }
    if (mysql_stmt_store_result(m_stmt) != 0) {
        setError(ErrorCode::DbQueryFailed, mysql_stmt_error(m_stmt));
        return false;
    }
    return true;
}

bool PreparedStatement::fetch() {
    if (!m_hasResult || m_stmt == NULL) {
        return false;
    }
    const int rc = mysql_stmt_fetch(m_stmt);
    if (rc == 0) {
        ++m_rows;
        return true;
    }
    if (rc == MYSQL_NO_DATA) {
        reportSlowIfNeeded();
        return false;
    }
    if (rc == MYSQL_DATA_TRUNCATED) {
        setError(ErrorCode::DbQueryFailed, "result column truncated");
    } else {
        const unsigned int code = mysql_stmt_errno(m_stmt);
        setError(ErrorCode::DbQueryFailed, mysql_stmt_error(m_stmt));
        if (m_conn != NULL && (code == 2003 || code == 2006 || code == 2013)) {
            m_conn->markBroken();
        }
    }
    reportSlowIfNeeded();
    return false;
}

bool PreparedStatement::isNull(std::size_t index) const {
    if (index >= m_fieldCount) {
        return true;
    }
    return m_resultNulls[index] != 0;
}

std::string PreparedStatement::getString(std::size_t index) const {
    if (index >= m_fieldCount || m_resultNulls[index] != 0) {
        return std::string();
    }
    return std::string(&m_resultBuffers[index][0],
                       static_cast<std::size_t>(m_resultLengths[index]));
}

std::int64_t PreparedStatement::getInt64(std::size_t index) const {
    const std::string text = getString(index);
    return std::strtoll(text.c_str(), NULL, 10);
}

std::uint64_t PreparedStatement::getUInt64(std::size_t index) const {
    const std::string text = getString(index);
    return std::strtoull(text.c_str(), NULL, 10);
}

std::uint64_t PreparedStatement::affectedRows() const {
    if (m_stmt == NULL) {
        return 0;
    }
    return static_cast<std::uint64_t>(mysql_stmt_affected_rows(m_stmt));
}

std::uint64_t PreparedStatement::insertId() const {
    if (m_stmt == NULL) {
        return 0;
    }
    return static_cast<std::uint64_t>(mysql_stmt_insert_id(m_stmt));
}

void PreparedStatement::reportSlowIfNeeded() {
    if (!m_timing || m_reported) {
        return;
    }
    m_reported = true;
    m_timing = false;

    const std::chrono::microseconds elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - m_startedAt);
    if (!isSlowQuery(elapsed, m_slowThreshold)) {
        return;
    }

    if (m_core != NULL) {
        m_core->slowQueryTotal.fetch_add(1, std::memory_order_relaxed);
        const std::uint64_t costMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
        std::uint64_t current = m_core->slowQueryMaxMs.load(std::memory_order_relaxed);
        while (costMs > current &&
               !m_core->slowQueryMaxMs.compare_exchange_weak(
                   current, costMs, std::memory_order_relaxed)) {
        }
    }

    SlowQueryInfo info;
    info.sql = m_sql;
    info.elapsed = elapsed;
    info.rows = m_rows;
    info.inTransaction = m_inTransaction;
    Logger::instance().log(LogLevel::Warn, __FILE__, __LINE__,
                           formatSlowQuery(info));
}

}  // namespace db
}  // namespace cm
