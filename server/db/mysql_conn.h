#pragma once

#include <mysql.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "cm/base/error_code.h"

namespace cm {
namespace db {

namespace detail {
struct PoolCore;
}

struct MysqlCloser {
    void operator()(MYSQL* handle) const {
        if (handle != nullptr) {
            mysql_close(handle);
        }
    }
};

typedef std::unique_ptr<MYSQL, MysqlCloser> MysqlHandle;

class PreparedStatement;

// 一条物理 MySQL 连接的 RAII 包装。
// 不拥有池；析构时把连接还给 PoolCore。生命周期必须短于创建它的 MysqlPool。
class MysqlConn {
public:
    ~MysqlConn();

    MysqlConn(const MysqlConn&) = delete;
    MysqlConn& operator=(const MysqlConn&) = delete;

    bool valid() const;
    bool ping();
    bool inTransaction() const;
    std::uint64_t id() const;

    std::unique_ptr<PreparedStatement> prepare(const std::string& sql);
    // 仅用于 DDL / 会话语句；禁止拼接用户输入。
    bool executeDdl(const std::string& sql);
    bool begin();
    bool commit();
    bool rollback();

    ErrorCode lastError() const { return m_lastError; }
    const std::string& lastErrorMessage() const { return m_lastErrorMsg; }

private:
    friend class MysqlPool;
    friend struct detail::PoolCore;
    friend class PreparedStatement;
    friend class Transaction;

    MysqlConn(const std::shared_ptr<detail::PoolCore>& core,
              MysqlHandle handle,
              std::chrono::steady_clock::time_point createdAt,
              std::chrono::milliseconds slowQueryThreshold);

    MYSQL* raw() const { return m_handle.get(); }
    void setError(ErrorCode code, const std::string& message);
    void setMysqlError(unsigned int code, const std::string& message);
    void markBroken();

    std::shared_ptr<detail::PoolCore> m_core;
    MysqlHandle m_handle;
    std::chrono::steady_clock::time_point m_createdAt;
    std::chrono::milliseconds m_slowQueryThreshold;
    bool m_broken;
    bool m_inTx;
    ErrorCode m_lastError;
    std::string m_lastErrorMsg;
};

// 事务 RAII：构造 BEGIN；析构时若未 commit/rollback 则回滚。
// 禁止嵌套；嵌套构造时 valid() 为 false。
class Transaction {
public:
    explicit Transaction(MysqlConn& conn);
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    bool valid() const { return m_valid; }
    bool commit();
    bool rollback();
    ErrorCode lastError() const { return m_conn.lastError(); }

private:
    MysqlConn& m_conn;
    bool m_valid;
    bool m_finished;
};

}  // namespace db
}  // namespace cm
