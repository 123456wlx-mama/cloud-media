#pragma once

#include <cstdint>

#include "cm/base/error_code.h"
#include "cm/protocol/messages/auth.h"

namespace cm {
namespace db {
class MysqlPool;
}  // namespace db

namespace logic {

// 注册 / 登录（10000 ~ 10003）。
// 密码存储：algo=1，u_password = md5Hex(u_salt + password)，salt 为 32 位 hex。
// 本阶段只支持 algo=1；登录失败统一 NotFound(1002)，不区分「不存在」与
// 「密码错」。
class AuthService {
public:
    static const std::uint8_t kPwdAlgoMd5 = 1;

    explicit AuthService(cm::db::MysqlPool& mysql);

    AuthService(const AuthService&) = delete;
    AuthService& operator=(const AuthService&) = delete;

    cm::ErrorCode registerUser(const cm::protocol::RegisterRq& rq,
                               cm::protocol::RegisterRs* out);

    cm::ErrorCode login(const cm::protocol::LoginRq& rq,
                        cm::protocol::LoginRs* out);

private:
    cm::db::MysqlPool& m_mysql;
};

}  // namespace logic
}  // namespace cm
