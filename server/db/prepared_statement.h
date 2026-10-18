#pragma once

#include <mysql.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "cm/base/error_code.h"

namespace cm {
namespace db {

namespace detail {
struct PoolCore;
}

class MysqlConn;

struct SlowQueryInfo {
    std::string sql;
    std::chrono::microseconds elapsed;
    std::uint64_t rows;
    bool inTransaction;
};

bool isSlowQuery(std::chrono::microseconds elapsed,
                 std::chrono::milliseconds threshold);
std::string formatSlowQuery(const SlowQueryInfo& info);

// MYSQL_STMT 的 RAII 封装。
// 只支持顺序读取结果；结果默认全部缓冲（store_result），适合元数据查询。
// 生命周期必须短于创建它的 MysqlConn。
class PreparedStatement {
public:
    ~PreparedStatement();

    PreparedStatement(const PreparedStatement&) = delete;
    PreparedStatement& operator=(const PreparedStatement&) = delete;

    // SQL 拼接辅助：只接受合法标识符，值一律用 ? 占位。
    static std::string placeholders(std::size_t count);
    static std::string buildInsert(const std::string& table,
                                   const std::vector<std::string>& columns);

    bool bindInt64(std::size_t index, std::int64_t value);
    bool bindUInt64(std::size_t index, std::uint64_t value);
    bool bindString(std::size_t index, const std::string& value);
    bool bindNull(std::size_t index);

    bool execute();

    enum class FetchStatus {
        Row,
        End,
        Error
    };

    FetchStatus fetchRow();
    bool fetch();
    bool isNull(std::size_t index) const;
    std::int64_t getInt64(std::size_t index) const;
    std::uint64_t getUInt64(std::size_t index) const;
    std::string getString(std::size_t index) const;

    std::uint64_t affectedRows() const;
    std::uint64_t insertId() const;

    const std::string& sql() const { return m_sql; }
    ErrorCode lastError() const { return m_lastError; }
    const std::string& lastErrorMessage() const { return m_lastErrorMsg; }

private:
    friend class MysqlConn;

    PreparedStatement(detail::PoolCore* core,
                      MysqlConn* conn,
                      MYSQL* mysql,
                      MYSQL_STMT* stmt,
                      const std::string& sql,
                      std::chrono::milliseconds slowThreshold,
                      bool inTransaction);

    void setError(ErrorCode code, const std::string& message);
    void setStmtError(const char* fallback);
    bool prepareResult();
    void reportSlowIfNeeded();

    detail::PoolCore* m_core;
    MysqlConn* m_conn;
    MYSQL* m_mysql;
    MYSQL_STMT* m_stmt;
    MYSQL_RES* m_meta;
    std::string m_sql;

    std::size_t m_paramCount;
    std::vector<MYSQL_BIND> m_paramBinds;
    std::vector<unsigned long> m_paramLengths;
    std::vector<my_bool> m_paramNulls;
    std::deque<std::string> m_paramStrings;
    std::vector<std::int64_t> m_intValues;
    std::vector<std::uint64_t> m_uintValues;

    std::size_t m_fieldCount;
    std::vector<MYSQL_BIND> m_resultBinds;
    std::vector<std::vector<char> > m_resultBuffers;
    std::vector<unsigned long> m_resultLengths;
    std::vector<my_bool> m_resultNulls;
    std::vector<my_bool> m_resultErrors;

    std::uint64_t m_rows;
    bool m_hasResult;
    bool m_timing;
    bool m_reported;
    std::chrono::steady_clock::time_point m_startedAt;
    std::chrono::milliseconds m_slowThreshold;
    bool m_inTransaction;
    ErrorCode m_lastError;
    std::string m_lastErrorMsg;
};

}  // namespace db
}  // namespace cm
