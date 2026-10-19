#include "logic/download_ticket.h"

#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <memory>

#include "cache/cache_store.h"
#include "cache/redis_pool.h"

namespace cm {
namespace logic {
namespace {

const std::uint64_t kTicketTtlMs = 5 * 60 * 1000;
const std::uint64_t kTicketHardMs = 2 * 60 * 60 * 1000;
// GET 比较与续期原子执行，缺键/被替换不续活；绝对过期由 Redis 时钟保证。
const char kTouchScript[] =
    "local value = redis.call('GET', KEYS[1]); "
    "if not value or value ~= ARGV[1] then return 0 end; "
    "local hard = string.match(value, '^[0-9]+|[0-9]+|([0-9]+)|[0-9]+$'); "
    "redis.call('PEXPIREAT', KEYS[1], hard); "
    "local remaining = redis.call('PTTL', KEYS[1]); "
    "if remaining <= 0 then return 0 end; "
    "redis.call('PEXPIRE', KEYS[1], math.min(remaining, 300000)); "
    "return 1";

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

bool isTicketHex(const std::string& value) {
    if (value.size() != 64) {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        const bool digit = (c >= '0' && c <= '9');
        const bool lower = (c >= 'a' && c <= 'f');
        if (!digit && !lower) {
            return false;
        }
    }
    return true;
}

// 直接读取系统密码学随机源，不能用小种子 PRNG 扩展授权凭证。
bool makeToken(std::string* token) {
    std::array<unsigned char, 32> bytes;
    std::ifstream random("/dev/urandom", std::ios::in | std::ios::binary);
    if (!random.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
        return false;
    }
    const char hex[] = "0123456789abcdef";
    token->clear();
    token->reserve(64);
    for (const auto byte : bytes) {
        token->push_back(hex[byte >> 4]);
        token->push_back(hex[byte & 0x0f]);
    }
    return true;
}

bool parseUnsigned(const std::string& text, std::uint64_t* value) {
    if (text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    *value = parsed;
    return true;
}

bool parseValue(const std::string& value, DownloadTicketInfo* out) {
    std::size_t first = value.find('|');
    if (first == std::string::npos) {
        return false;
    }
    std::size_t second = value.find('|', first + 1);
    if (second == std::string::npos) {
        return false;
    }
    std::size_t third = value.find('|', second + 1);
    if (third == std::string::npos ||
        value.find('|', third + 1) != std::string::npos) {
        return false;
    }

    std::uint64_t uid = 0;
    std::uint64_t fid = 0;
    std::uint64_t hardExp = 0;
    std::uint64_t flags = 0;
    if (!parseUnsigned(value.substr(0, first), &uid) ||
        !parseUnsigned(value.substr(first + 1, second - first - 1), &fid) ||
        !parseUnsigned(value.substr(second + 1, third - second - 1), &hardExp) ||
        !parseUnsigned(value.substr(third + 1), &flags) ||
        flags > 0xFFFFFFFFULL) {
        return false;
    }

    out->uid = uid;
    out->fid = fid;
    out->hardExpMs = hardExp;
    out->flags = static_cast<std::uint32_t>(flags);
    return true;
}

ErrorCode redisFailure(const cm::cache::RedisConn& conn) {
    const ErrorCode code = conn.lastError();
    return code == ErrorCode::AuthUnavailable ? code : ErrorCode::RedisUnavailable;
}

}  // namespace

DownloadTicketInfo::DownloadTicketInfo()
    : uid(0), fid(0), hardExpMs(0), flags(0) {
}

DownloadTicket::DownloadTicket(cm::cache::RedisPool& pool) : m_pool(pool) {
}

ErrorCode DownloadTicket::issue(std::uint64_t uid,
                                std::uint64_t fid,
                                std::string* token,
                                std::uint64_t* expireAtMs) {
    if (token == nullptr || expireAtMs == nullptr || uid == 0 || fid == 0) {
        return ErrorCode::InvalidArgument;
    }
    token->clear();
    *expireAtMs = 0;

    std::unique_ptr<cm::cache::RedisConn> conn = m_pool.acquire();
    if (!conn) {
        return ErrorCode::RedisUnavailable;
    }

    const std::uint64_t now = nowMs();
    const std::uint64_t hardExp = now + kTicketHardMs;
    std::string ticket;
    if (!makeToken(&ticket)) {
        return ErrorCode::InternalError;
    }
    const std::string value = std::to_string(uid) + "|" +
                              std::to_string(fid) + "|" +
                              std::to_string(hardExp) + "|0";

    if (!conn->setPx(cm::cache::CacheKey::downloadTicket(ticket), value,
                     std::chrono::milliseconds(kTicketTtlMs))) {
        return redisFailure(*conn);
    }

    *token = ticket;
    *expireAtMs = now + kTicketTtlMs;
    return ErrorCode::Ok;
}

ErrorCode DownloadTicket::loadWith(cm::cache::RedisConn& conn,
                                   const std::string& token,
                                   std::uint64_t uid,
                                   std::uint64_t fid,
                                   DownloadTicketInfo* out,
                                   std::string* rawValue) const {
    if (out == nullptr || uid == 0 || fid == 0 || !isTicketHex(token)) {
        return ErrorCode::PermissionDenied;
    }
    *out = DownloadTicketInfo();

    std::string value;
    const cm::cache::RedisLookup lookup =
        conn.get(cm::cache::CacheKey::downloadTicket(token), &value);
    if (lookup == cm::cache::RedisLookup::Error) {
        return redisFailure(conn);
    }
    if (lookup == cm::cache::RedisLookup::Miss) {
        return ErrorCode::PermissionDenied;
    }

    DownloadTicketInfo parsed;
    if (!parseValue(value, &parsed)) {
        return ErrorCode::PermissionDenied;
    }
    if (parsed.hardExpMs <= nowMs()) {
        return ErrorCode::PermissionDenied;
    }
    if (parsed.uid != uid || parsed.fid != fid) {
        return ErrorCode::PermissionDenied;
    }

    *out = parsed;
    if (rawValue != nullptr) {
        *rawValue = value;
    }
    return ErrorCode::Ok;
}

ErrorCode DownloadTicket::verify(const std::string& token,
                                 std::uint64_t uid,
                                 std::uint64_t fid,
                                 DownloadTicketInfo* out) {
    std::unique_ptr<cm::cache::RedisConn> conn = m_pool.acquire();
    if (!conn) {
        return ErrorCode::RedisUnavailable;
    }
    return loadWith(*conn, token, uid, fid, out);
}

ErrorCode DownloadTicket::touch(const std::string& token,
                                std::uint64_t uid,
                                std::uint64_t fid,
                                DownloadTicketInfo* out) {
    std::unique_ptr<cm::cache::RedisConn> conn = m_pool.acquire();
    if (!conn) {
        return ErrorCode::RedisUnavailable;
    }

    DownloadTicketInfo info;
    std::string value;
    const ErrorCode rc = loadWith(*conn, token, uid, fid, &info, &value);
    if (rc != ErrorCode::Ok) {
        return rc;
    }

    cm::cache::RedisReply reply;
    if (!conn->eval(kTouchScript, cm::cache::CacheKey::downloadTicket(token),
                    value, &reply)) {
        return redisFailure(*conn);
    }
    if (!reply.valid() || reply.type() != REDIS_REPLY_INTEGER) {
        return ErrorCode::RedisUnavailable;
    }
    if (reply.integer() != 1) {
        return ErrorCode::PermissionDenied;
    }

    if (out != nullptr) {
        *out = info;
    }
    return ErrorCode::Ok;
}

}  // namespace logic
}  // namespace cm
