#include "auth_service.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <memory>
#include <string>

#include "cm/base/md5.h"
#include "db/mysql_conn.h"
#include "db/mysql_pool.h"
#include "db/user_dao.h"

namespace cm {
namespace logic {
namespace {

// 与协议 / t_user 列宽一致。
const std::size_t kMaxTelBytes = 20;
const std::size_t kMaxPasswordBytes = 64;
const std::size_t kMaxNameBytes = 45;
const std::size_t kSaltBytes = 32;  // hex 文本长度，对应 16 随机字节

ErrorCode dbPoolError(cm::db::MysqlPool& pool) {
    return pool.lastError() == ErrorCode::Ok ? ErrorCode::DbPoolTimeout
                                             : pool.lastError();
}

bool isTelOk(const std::string& tel) {
    return !tel.empty() && tel.size() <= kMaxTelBytes;
}

bool isPasswordOk(const std::string& password) {
    return !password.empty() && password.size() <= kMaxPasswordBytes;
}

bool isNameOk(const std::string& name) {
    return !name.empty() && name.size() <= kMaxNameBytes;
}

// 32 位小写 hex：16 字节 /dev/urandom。
bool makeSalt(std::string* salt) {
    unsigned char raw[16];
    const int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return false;
    }
    std::size_t got = 0;
    while (got < sizeof(raw)) {
        const ssize_t n = ::read(fd, raw + got, sizeof(raw) - got);
        if (n <= 0) {
            ::close(fd);
            return false;
        }
        got += static_cast<std::size_t>(n);
    }
    ::close(fd);

    static const char kHex[] = "0123456789abcdef";
    salt->clear();
    salt->reserve(kSaltBytes);
    for (std::size_t i = 0; i < sizeof(raw); ++i) {
        salt->push_back(kHex[raw[i] >> 4]);
        salt->push_back(kHex[raw[i] & 0x0f]);
    }
    return true;
}

// 定长比较，避免按字节提前返回泄露匹配前缀。
bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

}  // namespace

const std::uint8_t AuthService::kPwdAlgoMd5;

AuthService::AuthService(cm::db::MysqlPool& mysql) : m_mysql(mysql) {
}

ErrorCode AuthService::registerUser(const cm::protocol::RegisterRq& rq,
                                    cm::protocol::RegisterRs* out) {
    if (out == nullptr) {
        return ErrorCode::InvalidArgument;
    }
    *out = cm::protocol::RegisterRs();

    if (!isTelOk(rq.tel) || !isPasswordOk(rq.password) ||
        !isNameOk(rq.name)) {
        out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        return ErrorCode::InvalidArgument;
    }

    std::string salt;
    if (!makeSalt(&salt)) {
        out->code = static_cast<std::uint32_t>(ErrorCode::InternalError);
        return ErrorCode::InternalError;
    }
    const std::string hash = cm::md5Hex(salt + rq.password);

    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        const ErrorCode code = dbPoolError(m_mysql);
        out->code = static_cast<std::uint32_t>(code);
        return code;
    }

    cm::db::UserDao dao(*conn);
    std::uint64_t uid = 0;
    const ErrorCode rc =
        dao.insert(rq.tel, hash, salt, kPwdAlgoMd5, rq.name, &uid);
    conn.reset();
    if (rc != ErrorCode::Ok) {
        out->code = static_cast<std::uint32_t>(cm::to_wire_code(rc));
        return rc;
    }

    out->uid = uid;
    out->code = 0;
    return ErrorCode::Ok;
}

ErrorCode AuthService::login(const cm::protocol::LoginRq& rq,
                             cm::protocol::LoginRs* out) {
    if (out == nullptr) {
        return ErrorCode::InvalidArgument;
    }
    *out = cm::protocol::LoginRs();

    if (!isTelOk(rq.tel) || !isPasswordOk(rq.password)) {
        out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        return ErrorCode::InvalidArgument;
    }

    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        const ErrorCode code = dbPoolError(m_mysql);
        out->code = static_cast<std::uint32_t>(code);
        return code;
    }

    cm::db::UserDao dao(*conn);
    cm::db::UserRow row;
    const ErrorCode rc = dao.findByTel(rq.tel, &row);
    conn.reset();
    if (rc != ErrorCode::Ok) {
        if (rc == ErrorCode::NotFound) {
            // 与密码错同码，不泄露账号是否存在。
            out->code = static_cast<std::uint32_t>(ErrorCode::NotFound);
            return ErrorCode::NotFound;
        }
        out->code = static_cast<std::uint32_t>(cm::to_wire_code(rc));
        return rc;
    }

    if (row.status != 0) {
        out->code = static_cast<std::uint32_t>(ErrorCode::PermissionDenied);
        return ErrorCode::PermissionDenied;
    }
    // 本阶段只实现 algo=1；遇到未实现的算法 fail-closed。
    if (row.pwdAlgo != kPwdAlgoMd5 || row.salt.empty()) {
        out->code = static_cast<std::uint32_t>(ErrorCode::AuthUnavailable);
        return ErrorCode::AuthUnavailable;
    }

    const std::string expected = cm::md5Hex(row.salt + rq.password);
    if (!constantTimeEquals(expected, row.passwordHash)) {
        out->code = static_cast<std::uint32_t>(ErrorCode::NotFound);
        return ErrorCode::NotFound;
    }

    out->uid = row.uid;
    out->name = row.name;
    out->code = 0;
    return ErrorCode::Ok;
}

}  // namespace logic
}  // namespace cm
