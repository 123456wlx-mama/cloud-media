#include "user_dao.h"

#include <memory>

#include "mysql_conn.h"
#include "prepared_statement.h"

namespace cm {
namespace db {
namespace {

bool isDuplicateEntry(const std::string& message) {
    return message.find("Duplicate entry") != std::string::npos ||
           message.find("ER_DUP_ENTRY") != std::string::npos;
}

// affectedRows() 在未开 CLIENT_FOUND_ROWS 的连接上返回 changed rows，
// 值未变化时为 0，无法与“行不存在”区分；需要显式再查一次。
ErrorCode userExists(MysqlConn& conn, std::uint64_t uid, bool* exists) {
    if (exists == NULL) {
        return ErrorCode::InvalidArgument;
    }
    *exists = false;

    std::unique_ptr<PreparedStatement> stmt =
        conn.prepare("SELECT u_id FROM t_user WHERE u_id = ?");
    if (!stmt) {
        return conn.lastError();
    }
    if (!stmt->bindUInt64(0, uid)) {
        return stmt->lastError();
    }
    if (!stmt->execute()) {
        return stmt->lastError();
    }

    const PreparedStatement::FetchStatus status = stmt->fetchRow();
    if (status == PreparedStatement::FetchStatus::Error) {
        return stmt->lastError();
    }
    *exists = (status == PreparedStatement::FetchStatus::Row);
    return ErrorCode::Ok;
}

}  // namespace

UserRow::UserRow()
    : uid(0), pwdAlgo(0), status(0) {
}

UserDao::UserDao(MysqlConn& conn) : m_conn(conn) {
}

bool UserDao::isSupportedPwdAlgo(std::uint8_t pwdAlgo) {
    return pwdAlgo == 1 || pwdAlgo == 2;
}

ErrorCode UserDao::findByTel(const std::string& tel, UserRow* out) {
    if (out == NULL || tel.empty()) {
        return ErrorCode::InvalidArgument;
    }

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "SELECT u_id, u_tel, u_password, u_salt, u_pwd_algo, u_name, u_status "
        "FROM t_user WHERE u_tel = ?");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindString(0, tel)) {
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

    out->uid = stmt->getUInt64(0);
    out->tel = stmt->getString(1);
    out->passwordHash = stmt->getString(2);
    out->salt = stmt->getString(3);
    out->pwdAlgo = static_cast<std::uint8_t>(stmt->getUInt64(4));
    out->name = stmt->getString(5);
    out->status = static_cast<std::uint8_t>(stmt->getUInt64(6));
    return ErrorCode::Ok;
}

ErrorCode UserDao::insert(const std::string& tel,
                          const std::string& passwordHash,
                          const std::string& salt,
                          std::uint8_t pwdAlgo,
                          const std::string& name,
                          std::uint64_t* uid) {
    if (uid == NULL || tel.empty() || passwordHash.empty() || salt.empty() ||
        !isSupportedPwdAlgo(pwdAlgo)) {
        return ErrorCode::InvalidArgument;
    }

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "INSERT INTO t_user (u_tel, u_password, u_salt, u_pwd_algo, u_name) "
        "VALUES (?, ?, ?, ?, ?)");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindString(0, tel) ||
        !stmt->bindString(1, passwordHash) ||
        !stmt->bindString(2, salt) ||
        !stmt->bindUInt64(3, pwdAlgo) ||
        !stmt->bindString(4, name)) {
        return stmt->lastError();
    }
    if (!stmt->execute()) {
        if (isDuplicateEntry(stmt->lastErrorMessage())) {
            return ErrorCode::AlreadyExists;
        }
        return stmt->lastError();
    }

    *uid = stmt->insertId();
    return ErrorCode::Ok;
}

ErrorCode UserDao::updatePassword(std::uint64_t uid,
                                  const std::string& passwordHash,
                                  std::uint8_t pwdAlgo,
                                  const std::string& salt) {
    if (uid == 0 || passwordHash.empty() || salt.empty() ||
        !isSupportedPwdAlgo(pwdAlgo)) {
        return ErrorCode::InvalidArgument;
    }

    std::unique_ptr<PreparedStatement> stmt = m_conn.prepare(
        "UPDATE t_user SET u_password = ?, u_pwd_algo = ?, u_salt = ? "
        "WHERE u_id = ?");
    if (!stmt) {
        return m_conn.lastError();
    }
    if (!stmt->bindString(0, passwordHash) ||
        !stmt->bindUInt64(1, pwdAlgo) ||
        !stmt->bindString(2, salt) ||
        !stmt->bindUInt64(3, uid)) {
        return stmt->lastError();
    }
    if (!stmt->execute()) {
        return stmt->lastError();
    }
    if (stmt->affectedRows() > 0) {
        return ErrorCode::Ok;
    }

    // 更新同值（例如只换盐不改算法）时 changed rows 为 0，不能直接判 NotFound。
    bool exists = false;
    const ErrorCode lookup = userExists(m_conn, uid, &exists);
    if (lookup != ErrorCode::Ok) {
        return lookup;
    }
    return exists ? ErrorCode::Ok : ErrorCode::NotFound;
}

}  // namespace db
}  // namespace cm
