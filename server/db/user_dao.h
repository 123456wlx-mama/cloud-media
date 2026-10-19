#pragma once

#include <cstdint>
#include <string>

#include "cm/base/error_code.h"

namespace cm {
namespace db {

class MysqlConn;

struct UserRow {
    std::uint64_t uid;
    std::string tel;
    std::string passwordHash;
    std::string salt;
    std::uint8_t pwdAlgo;
    std::string name;
    std::uint8_t status;

    UserRow();
};

// 登录/注册最小 DAO：不持有连接池，调用方用 MysqlPool::acquire 取得连接后构造。
// 所有 SQL 均为预处理语句；本类不拼接用户输入。
class UserDao {
public:
    explicit UserDao(MysqlConn& conn);

    UserDao(const UserDao&) = delete;
    UserDao& operator=(const UserDao&) = delete;

    ErrorCode findByTel(const std::string& tel, UserRow* out);
    ErrorCode insert(const std::string& tel,
                     const std::string& passwordHash,
                     const std::string& salt,
                     std::uint8_t pwdAlgo,
                     const std::string& name,
                     std::uint64_t* uid);
    ErrorCode updatePassword(std::uint64_t uid,
                             const std::string& passwordHash,
                             std::uint8_t pwdAlgo,
                             const std::string& salt);

    static bool isSupportedPwdAlgo(std::uint8_t pwdAlgo);

private:
    MysqlConn& m_conn;
};

}  // namespace db
}  // namespace cm
