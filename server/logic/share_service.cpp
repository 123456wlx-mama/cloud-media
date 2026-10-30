#include "share_service.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <memory>
#include <random>
#include <string>

#include "cache/cache_store.h"
#include "cache/redis_pool.h"
#include "db/file_dao.h"
#include "db/mysql_conn.h"
#include "db/mysql_pool.h"
#include "db/share_dao.h"

namespace cm {
namespace logic {
namespace {

const std::uint64_t kShareCacheTtlMs = 5 * 60 * 1000;       // share/{code} 5min±10%
const std::uint64_t kShareMissTtlMs = 60 * 1000;            // miss 60s
const std::uint64_t kReqResultTtlMs = 10 * 60 * 1000;       // req 幂等结果 10min
const std::uint64_t kShareTicketTtlMs = 10 * 60 * 1000;     // share_ticket 10min
const std::uint64_t kAccessCodeFailLimit = 5;               // 5 次/10min 锁定
const char kAccessCodeFailWindowMs[] = "600000";            // 锁定窗口 10min
const int kAccessCodeMin = 1000;
const int kAccessCodeMax = 9999;

// INCR + 首次 PEXPIRE，固定窗口计数（05 §5.6）。
const char kFailCountScript[] =
    "local v = redis.call('INCR', KEYS[1]); "
    "if v == 1 then redis.call('PEXPIRE', KEYS[1], ARGV[1]) end; "
    "return v";

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::mt19937& rng() {
    static std::mt19937 engine = []() {
        std::random_device rd;
        std::mt19937::result_type seeds[4];
        for (int i = 0; i < 4; ++i) {
            seeds[i] = rd();
        }
        std::seed_seq seq(seeds, seeds + 4);
        return std::mt19937(seq);
    }();
    return engine;
}

std::string makeAccessCode() {
    std::uniform_int_distribution<int> dist(kAccessCodeMin, kAccessCodeMax);
    return std::to_string(dist(rng()));
}

// 直接读取系统随机源扩展 share_ticket，不能用小种子 PRNG 生成授权凭证。
bool makeToken(std::string* token) {
    std::array<unsigned char, 32> bytes;
    std::ifstream random("/dev/urandom", std::ios::in | std::ios::binary);
    if (!random.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
        return false;
    }
    const char hex[] = "0123456789abcdef";
    token->clear();
    token->reserve(64);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        token->push_back(hex[bytes[i] >> 4]);
        token->push_back(hex[bytes[i] & 0x0f]);
    }
    return true;
}

bool parseUnsigned(const std::string& text, std::uint64_t* value) {
    if (value == nullptr || text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
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

// 输入允许小写与空格，服务端归一化为大写去空格（06 §1.3）。
std::string normalizeCode(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == ' ' || c == '\t') {
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        out.push_back(c);
    }
    return out;
}

// 1970-01-01 起的天数（civil 日期算法，纯整数，避免依赖 timegm）。
std::int64_t daysFromCivil(int year, unsigned month, unsigned day) {
    year -= (month <= 2);
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);  // [0, 399]
    const unsigned doy =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;  // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;     // [0, 146096]
    return static_cast<std::int64_t>(era) * 146097 +
           static_cast<std::int64_t>(doe) - 719468;
}

// epoch 毫秒 → MySQL DATETIME(6) 文本（UTC），用于列表游标回填。
std::string msToDatetime(std::uint64_t ms) {
    const std::time_t seconds = static_cast<std::time_t>(ms / 1000ULL);
    std::tm tm_value;
    std::memset(&tm_value, 0, sizeof(tm_value));
    ::gmtime_r(&seconds, &tm_value);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer),
                  "%04d-%02d-%02d %02d:%02d:%02d.%06d",
                  tm_value.tm_year + 1900, tm_value.tm_mon + 1,
                  tm_value.tm_mday, tm_value.tm_hour, tm_value.tm_min,
                  tm_value.tm_sec,
                  static_cast<int>((ms % 1000ULL) * 1000ULL));
    return std::string(buffer);
}

// MySQL DATETIME(6) 文本 "YYYY-MM-DD HH:MM:SS.ffffff" → epoch 毫秒（UTC）。
// 解析失败返回 0（与「永久」的 0 重合，但调用方对永久分享不走到这里）。
std::uint64_t datetimeToMs(const std::string& text) {
    if (text.size() < 19) {
        return 0;
    }
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day,
                    &hour, &minute, &second) != 6) {
        return 0;
    }
    if (year < 1970) {
        return 0;
    }
    std::uint64_t ms = 0;
    if (text.size() > 19 && text[19] == '.') {
        std::uint64_t micros = 0;
        for (std::size_t i = 20; i < text.size() && i < 26; ++i) {
            const char c = text[i];
            if (c < '0' || c > '9') {
                break;
            }
            micros = micros * 10 + static_cast<std::uint64_t>(c - '0');
        }
        ms = micros / 1000;
    }
    const std::int64_t days =
        daysFromCivil(year, static_cast<unsigned>(month),
                      static_cast<unsigned>(day));
    return static_cast<std::uint64_t>(days) * 86400000ULL +
           static_cast<std::uint64_t>(hour) * 3600000ULL +
           static_cast<std::uint64_t>(minute) * 60000ULL +
           static_cast<std::uint64_t>(second) * 1000ULL + ms;
}

bool isPermanent(const std::string& expireAt) {
    return expireAt.compare(0, 4, "9999") == 0;
}

std::uint64_t expireAtToMs(const std::string& expireAt) {
    return isPermanent(expireAt) ? 0 : datetimeToMs(expireAt);
}

// 缓存元信息：pipe 拼接，accessCode 放最后（4 位数字，不含 '|'）。
struct ShareMeta {
    std::uint8_t status;
    std::uint64_t expireAtMs;
    std::uint64_t userId;
    std::uint64_t ufId;
    std::uint64_t fileId;  // 目录为 0
    bool accessRequired;
    std::string accessCode;

    ShareMeta()
        : status(0),
          expireAtMs(0),
          userId(0),
          ufId(0),
          fileId(0),
          accessRequired(false) {
    }
};

std::string encodeMeta(const ShareMeta& meta) {
    return std::to_string(meta.status) + "|" +
           std::to_string(meta.expireAtMs) + "|" +
           std::to_string(meta.userId) + "|" + std::to_string(meta.ufId) + "|" +
           std::to_string(meta.fileId) + "|" +
           std::to_string(meta.accessRequired ? 1 : 0) + "|" + meta.accessCode;
}

bool parseMeta(const std::string& value, ShareMeta* out) {
    if (out == nullptr) {
        return false;
    }
    std::size_t pos[6];
    std::size_t start = 0;
    for (int i = 0; i < 6; ++i) {
        pos[i] = value.find('|', start);
        if (pos[i] == std::string::npos) {
            return false;
        }
        start = pos[i] + 1;
    }
    if (value.find('|', start) != std::string::npos) {
        return false;  // accessCode 中不应再出现 '|'
    }

    std::uint64_t status = 0, expireAtMs = 0, userId = 0, ufId = 0, fileId = 0,
                 accessReq = 0;
    if (!parseUnsigned(value.substr(0, pos[0]), &status) ||
        !parseUnsigned(value.substr(pos[0] + 1, pos[1] - pos[0] - 1),
                       &expireAtMs) ||
        !parseUnsigned(value.substr(pos[1] + 1, pos[2] - pos[1] - 1), &userId) ||
        !parseUnsigned(value.substr(pos[2] + 1, pos[3] - pos[2] - 1), &ufId) ||
        !parseUnsigned(value.substr(pos[3] + 1, pos[4] - pos[3] - 1), &fileId) ||
        !parseUnsigned(value.substr(pos[4] + 1, pos[5] - pos[4] - 1),
                       &accessReq)) {
        return false;
    }
    if (status > 1 || accessReq > 1) {
        return false;
    }

    out->status = static_cast<std::uint8_t>(status);
    out->expireAtMs = expireAtMs;
    out->userId = userId;
    out->ufId = ufId;
    out->fileId = fileId;
    out->accessRequired = accessReq != 0;
    out->accessCode = value.substr(start);
    if (out->accessRequired && out->accessCode.size() != 4) {
        return false;
    }
    return true;
}

ErrorCode dbPoolError(cm::db::MysqlPool& pool) {
    return pool.lastError() == ErrorCode::Ok ? ErrorCode::DbPoolTimeout
                                             : pool.lastError();
}

// 提取码校验（fail-closed）：Redis 挂回 4008；5 次错误后锁定，锁定期间不比较。
ErrorCode verifyAccessCode(cm::cache::RedisPool& pool,
                           const std::string& code,
                           const std::string& provided,
                           const std::string& correct,
                           const std::string& clientKey) {
    std::unique_ptr<cm::cache::RedisConn> conn = pool.acquire();
    if (!conn) {
        return ErrorCode::RateLimitUnavailable;
    }
    const std::string failKey =
        cm::cache::CacheKey::shareFail(code, clientKey);
    if (failKey.empty()) {
        return ErrorCode::RateLimitUnavailable;
    }

    std::uint64_t fails = 0;
    std::string value;
    const cm::cache::RedisLookup lookup = conn->get(failKey, &value);
    if (lookup == cm::cache::RedisLookup::Error) {
        return ErrorCode::RateLimitUnavailable;
    }
    if (lookup == cm::cache::RedisLookup::Hit) {
        std::uint64_t n = 0;
        fails = parseUnsigned(value, &n) ? n : 0;
    }

    if (fails >= kAccessCodeFailLimit) {
        return ErrorCode::AccessCodeWrong;  // 锁定期间直接 2004，不查也不比较
    }
    if (provided == correct) {
        conn->del(failKey);  // best-effort 重置，失败不影响通过
        return ErrorCode::Ok;
    }

    cm::cache::RedisReply reply;
    if (!conn->eval(kFailCountScript, failKey, kAccessCodeFailWindowMs, &reply) ||
        !reply.valid() || reply.type() != REDIS_REPLY_INTEGER) {
        return ErrorCode::RateLimitUnavailable;
    }
    return ErrorCode::AccessCodeWrong;
}

// share_ticket best-effort：Redis 挂返回空串，不阻断 open（下载侧 fail-closed）。
std::string issueShareTicket(cm::cache::RedisPool& pool,
                             std::uint64_t ownerUid,
                             std::uint64_t fid,
                             const std::string& code) {
    std::unique_ptr<cm::cache::RedisConn> conn = pool.acquire();
    if (!conn) {
        return std::string();
    }
    std::string token;
    if (!makeToken(&token)) {
        return std::string();
    }
    const std::string value =
        std::to_string(ownerUid) + "|" + std::to_string(fid) + "|" + code;
    if (!conn->setPx(cm::cache::CacheKey::shareTicket(token), value,
                     std::chrono::milliseconds(kShareTicketTtlMs))) {
        return std::string();
    }
    return token;
}

// 分享解析（10022 访问 / 10044 转存共用）：
//   码归一化 → 缓存(fail-open) → 回源 → 状态/过期 → 提取码(fail-closed)。
// 成功返回 Ok 且 meta 有效；失败时 *wireCode 为回给客户端的错误码。
ErrorCode resolveShare(cm::cache::CacheStore& cache,
                       cm::db::MysqlPool& mysql,
                       cm::cache::RedisPool& redis,
                       const std::string& rawCode,
                       const std::string& accessCode,
                       const std::string& clientKey,
                       ShareMeta* meta,
                       std::uint32_t* wireCode) {
    if (meta == nullptr || wireCode == nullptr) {
        return ErrorCode::InvalidArgument;
    }

    const std::string code = normalizeCode(rawCode);
    if (!cm::db::ShareDao::isValidCode(code)) {
        *wireCode = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        return ErrorCode::InvalidArgument;
    }

    // 缓存 → 回源（fail-open）：share/{code} 命中直接用，miss 命中直接 1002。
    const std::string shareKey = cm::cache::CacheKey::share(code);
    const std::string missKey = cm::cache::CacheKey::shareMiss(code);

    bool haveMeta = false;
    std::string cachedValue;
    const cm::cache::CacheStore::Result cachedResult =
        cache.get(shareKey, &cachedValue);
    if (cachedResult == cm::cache::CacheStore::Result::Hit) {
        haveMeta = parseMeta(cachedValue, meta);
    }
    if (!haveMeta) {
        if (cachedResult == cm::cache::CacheStore::Result::Miss) {
            std::string missValue;
            if (cache.get(missKey, &missValue) ==
                cm::cache::CacheStore::Result::Hit) {
                *wireCode = static_cast<std::uint32_t>(ErrorCode::NotFound);
                return ErrorCode::NotFound;
            }
        }

        std::unique_ptr<cm::db::MysqlConn> conn = mysql.acquire();
        if (!conn) {
            const ErrorCode err = dbPoolError(mysql);
            *wireCode = static_cast<std::uint32_t>(err);
            return err;
        }
        cm::db::ShareDao shareDao(*conn);
        cm::db::ShareRow row;
        const ErrorCode rc = shareDao.findByCode(code, &row);
        if (rc == ErrorCode::NotFound) {
            conn.reset();
            cache.put(missKey, "1", std::chrono::milliseconds(kShareMissTtlMs));
            *wireCode = static_cast<std::uint32_t>(ErrorCode::NotFound);
            return ErrorCode::NotFound;
        }
        if (rc != ErrorCode::Ok) {
            conn.reset();
            *wireCode = static_cast<std::uint32_t>(rc);
            return rc;
        }
        conn.reset();

        meta->status = row.status;
        meta->expireAtMs = expireAtToMs(row.expireAt);
        meta->userId = row.userId;
        meta->ufId = row.ufId;
        meta->fileId = row.fileId;
        meta->accessRequired = !row.accessCode.empty();
        meta->accessCode = row.accessCode;
        haveMeta = true;

        cache.put(shareKey, encodeMeta(*meta),
                  std::chrono::milliseconds(kShareCacheTtlMs));
    }

    if (meta->status != 0) {
        *wireCode = static_cast<std::uint32_t>(ErrorCode::ShareExpired);
        return ErrorCode::ShareExpired;
    }
    if (meta->expireAtMs != 0 && meta->expireAtMs < nowMs()) {
        *wireCode = static_cast<std::uint32_t>(ErrorCode::ShareExpired);
        return ErrorCode::ShareExpired;
    }

    // 提取码（fail-closed）
    if (meta->accessRequired) {
        const ErrorCode rc =
            verifyAccessCode(redis, code, accessCode, meta->accessCode, clientKey);
        if (rc != ErrorCode::Ok) {
            *wireCode = static_cast<std::uint32_t>(rc);
            return rc;
        }
    }
    return ErrorCode::Ok;
}

}  // namespace

std::string ShareService::generateCode() {
    const char* alphabet = cm::db::ShareDao::codeAlphabet();
    const std::size_t alphabetSize = 31;
    std::uniform_int_distribution<int> dist(0,
                                            static_cast<int>(alphabetSize) - 1);
    std::string code(6, '0');
    for (std::size_t i = 0; i < code.size(); ++i) {
        code[i] = alphabet[static_cast<std::size_t>(dist(rng()))];
    }
    return code;
}

ShareService::ShareService(cm::db::MysqlPool& mysql,
                           cm::cache::CacheStore& cache,
                           cm::cache::RedisPool& redis,
                           ShareCodeGenerator codeGen)
    : m_mysql(mysql), m_cache(cache), m_redis(redis), m_codeGen(codeGen) {
}

std::string ShareService::nextCode() const {
    return m_codeGen ? m_codeGen() : generateCode();
}

ErrorCode ShareService::create(std::uint64_t uid,
                               const cm::protocol::ShareCreateRq& rq,
                               cm::protocol::ShareCreateRs* out) {
    if (out == nullptr) {
        return ErrorCode::InvalidArgument;
    }
    *out = cm::protocol::ShareCreateRs();

    if (uid == 0 || rq.ufId == 0 || rq.expirePolicy > 2 || rq.accessMode > 1) {
        out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        return ErrorCode::InvalidArgument;
    }

    // req 幂等（fail-open：Redis 不可用允许重复创建，uk_s_code 兜底不损坏数据）。
    const std::string resultKey = cm::cache::CacheKey::shareReq(uid, rq.reqId);
    std::string cached;
    if (m_cache.get(resultKey, &cached) == cm::cache::CacheStore::Result::Hit) {
        if (out->decode(cached)) {
            return ErrorCode::Ok;
        }
        // 缓存脏数据：继续走完整流程，下次 put 覆盖。
    }

    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        const ErrorCode code = dbPoolError(m_mysql);
        out->code = static_cast<std::uint32_t>(code);
        return code;
    }

    cm::db::FileDao fileDao(*conn);
    cm::db::UserFileRow node;
    ErrorCode rc = fileDao.findUserFile(uid, rq.ufId, &node);
    if (rc == ErrorCode::NotFound ||
        (rc == ErrorCode::Ok &&
         node.deletedAt != cm::db::FileDao::activeDeletedAt())) {
        conn.reset();
        out->code = static_cast<std::uint32_t>(ErrorCode::NotFound);
        return ErrorCode::NotFound;
    }
    if (rc != ErrorCode::Ok) {
        conn.reset();
        out->code = static_cast<std::uint32_t>(rc);
        return rc;
    }

    const bool isDir = node.isDir;
    const std::uint64_t fileId = node.fid;
    const std::string accessCode =
        rq.accessMode == 1 ? makeAccessCode() : std::string();

    // 生成码 + INSERT 捕获 1062 重试 5 次（DAO 不吞 1062，抛 AlreadyExists）。
    cm::db::ShareDao shareDao(*conn);
    std::string code;
    std::uint64_t shareId = 0;
    ErrorCode insertRc = ErrorCode::AlreadyExists;
    for (int attempt = 0; attempt < 5 && insertRc == ErrorCode::AlreadyExists;
         ++attempt) {
        code = nextCode();
        cm::db::NewShare share;
        share.code = code;
        share.userId = uid;
        share.ufId = rq.ufId;
        share.fileId = fileId;
        share.isDir = isDir;
        share.accessCode = accessCode;
        share.expirePolicy = rq.expirePolicy;
        insertRc = shareDao.insert(share, &shareId);
    }
    conn.reset();
    if (insertRc != ErrorCode::Ok) {
        out->code = static_cast<std::uint32_t>(insertRc);
        return insertRc;
    }

    std::uint64_t expireAtMs = 0;
    if (rq.expirePolicy == 1) {
        expireAtMs = nowMs() + 24 * 60 * 60 * 1000ULL;
    } else if (rq.expirePolicy == 2) {
        expireAtMs = nowMs() + 7 * 24 * 60 * 60 * 1000ULL;
    }

    out->code = 0;
    out->shareCode = code;
    out->accessCode = accessCode;
    out->accessCodeRequired = rq.accessMode == 1 ? 1 : 0;
    out->expireAtMs = expireAtMs;

    ShareMeta meta;
    meta.status = 0;
    meta.expireAtMs = expireAtMs;
    meta.userId = uid;
    meta.ufId = rq.ufId;
    meta.fileId = fileId;
    meta.accessRequired = rq.accessMode == 1;
    meta.accessCode = accessCode;
    m_cache.put(cm::cache::CacheKey::share(code), encodeMeta(meta),
                std::chrono::milliseconds(kShareCacheTtlMs));

    std::string payload;
    if (out->encode(&payload)) {
        m_cache.put(resultKey, payload, std::chrono::milliseconds(kReqResultTtlMs));
    }
    return ErrorCode::Ok;
}

ErrorCode ShareService::open(const cm::protocol::ShareOpenRq& rq,
                             const std::string& clientKey,
                             cm::protocol::ShareOpenRs* out) {
    if (out == nullptr) {
        return ErrorCode::InvalidArgument;
    }
    *out = cm::protocol::ShareOpenRs();

    ShareMeta meta;
    std::uint32_t wireCode = 0;
    const ErrorCode resolved =
        resolveShare(m_cache, m_mysql, m_redis, rq.shareCode, rq.accessCode,
                     clientKey, &meta, &wireCode);
    if (resolved != ErrorCode::Ok) {
        out->code = wireCode;
        return resolved;
    }

    // 节点（fileName/isDir）与 blob（size/md5）信息
    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        const ErrorCode err = dbPoolError(m_mysql);
        out->code = static_cast<std::uint32_t>(err);
        return err;
    }
    cm::db::FileDao fileDao(*conn);
    cm::db::UserFileRow node;
    ErrorCode rc = fileDao.findUserFile(meta.userId, meta.ufId, &node);
    if (rc == ErrorCode::NotFound ||
        (rc == ErrorCode::Ok &&
         node.deletedAt != cm::db::FileDao::activeDeletedAt())) {
        conn.reset();
        out->code = static_cast<std::uint32_t>(ErrorCode::NotFound);
        return ErrorCode::NotFound;
    }
    if (rc != ErrorCode::Ok) {
        conn.reset();
        out->code = static_cast<std::uint32_t>(rc);
        return rc;
    }

    std::uint64_t size = 0;
    std::string md5;
    if (node.fid != 0) {
        cm::db::BlobRow blob;
        rc = fileDao.getBlobById(node.fid, &blob);
        if (rc == ErrorCode::NotFound) {
            conn.reset();
            out->code = static_cast<std::uint32_t>(ErrorCode::FileGone);
            return ErrorCode::FileGone;
        }
        if (rc != ErrorCode::Ok) {
            conn.reset();
            out->code = static_cast<std::uint32_t>(rc);
            return rc;
        }
        size = blob.size;
        md5 = blob.md5;
    }
    conn.reset();

    out->code = 0;
    out->fId = node.fid;
    out->fileName = node.name;
    out->size = size;
    out->fileMd5 = md5;
    out->isDir = node.isDir ? 1 : 0;
    out->expireAtMs = meta.expireAtMs;
    out->shareTicket = issueShareTicket(m_redis, meta.userId, node.fid,
                                        normalizeCode(rq.shareCode));
    out->accessCodeRequired = meta.accessRequired ? 1 : 0;
    return ErrorCode::Ok;
}

ErrorCode ShareService::list(std::uint64_t uid,
                              const cm::protocol::ShareListRq& rq,
                              cm::protocol::ShareListRs* out) {
    if (out == nullptr) {
        return ErrorCode::InvalidArgument;
    }
    *out = cm::protocol::ShareListRs();

    if (uid == 0 || rq.limit == 0 || rq.limit > 200) {
        out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        return ErrorCode::InvalidArgument;
    }
    const bool firstPage = rq.cursorCreatedMs == 0 && rq.cursorId == 0;
    if (!firstPage && (rq.cursorCreatedMs == 0 || rq.cursorId == 0)) {
        out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        return ErrorCode::InvalidArgument;
    }

    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        const ErrorCode code = dbPoolError(m_mysql);
        out->code = static_cast<std::uint32_t>(code);
        return code;
    }

    cm::db::ShareListQuery query;
    query.userId = uid;
    query.limit = rq.limit;
    if (!firstPage) {
        query.cursorCreatedAt = msToDatetime(rq.cursorCreatedMs);
        query.cursorId = rq.cursorId;
    }

    cm::db::ShareDao shareDao(*conn);
    cm::db::ShareListPage page;
    ErrorCode rc = shareDao.listByUser(query, &page);
    if (rc != ErrorCode::Ok) {
        conn.reset();
        out->code = static_cast<std::uint32_t>(rc);
        return rc;
    }

    cm::db::FileDao fileDao(*conn);
    out->items.reserve(page.rows.size());
    for (std::size_t i = 0; i < page.rows.size(); ++i) {
        const cm::db::ShareRow& row = page.rows[i];
        cm::protocol::ShareListItem item;
        item.shareCode = row.code;
        item.size = 0;
        item.accessCodeRequired = row.accessCode.empty() ? 0 : 1;
        item.status = row.status;
        item.expireAtMs = datetimeToMs(row.expireAt);
        item.createdAtMs = datetimeToMs(row.createdAt);

        cm::db::UserFileRow node;
        if (fileDao.findUserFile(row.userId, row.ufId, &node) ==
            ErrorCode::Ok) {
            item.fileName = node.name;
            if (node.fid != 0) {
                cm::db::BlobRow blob;
                if (fileDao.getBlobById(node.fid, &blob) == ErrorCode::Ok) {
                    item.size = blob.size;
                }
            }
        }
        out->items.push_back(item);
    }
    conn.reset();

    out->code = 0;
    out->hasMore = page.hasMore ? 1 : 0;
    out->nextCreatedMs = datetimeToMs(page.nextCreatedAt);
    out->nextId = page.nextId;
    return ErrorCode::Ok;
}

ErrorCode ShareService::cancel(std::uint64_t uid,
                               const cm::protocol::ShareCancelRq& rq,
                               cm::protocol::ShareCancelRs* out) {
    if (out == nullptr) {
        return ErrorCode::InvalidArgument;
    }
    *out = cm::protocol::ShareCancelRs();

    const std::string code = normalizeCode(rq.shareCode);
    if (!cm::db::ShareDao::isValidCode(code)) {
        out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        return ErrorCode::InvalidArgument;
    }

    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        const ErrorCode err = dbPoolError(m_mysql);
        out->code = static_cast<std::uint32_t>(err);
        return err;
    }
    cm::db::ShareDao shareDao(*conn);
    bool cancelled = false;
    bool already = false;
    const ErrorCode rc = shareDao.cancel(uid, code, &cancelled, &already);
    conn.reset();
    if (rc != ErrorCode::Ok) {
        out->code = static_cast<std::uint32_t>(rc);
        return rc;
    }

    // 取消后失效缓存；already 时也失效一次，覆盖脏缓存。
    m_cache.invalidate(cm::cache::CacheKey::share(code));

    out->code = 0;
    out->cancelled = cancelled ? 1 : 0;
    out->already = already ? 1 : 0;
    return ErrorCode::Ok;
}

ErrorCode ShareService::save(std::uint64_t uid,
                             const cm::protocol::ShareSaveRq& rq,
                             const std::string& clientKey,
                             cm::protocol::ShareSaveRs* out) {
    if (out == nullptr) {
        return ErrorCode::InvalidArgument;
    }
    *out = cm::protocol::ShareSaveRs();
    if (uid == 0) {
        out->code = static_cast<std::uint32_t>(ErrorCode::PermissionDenied);
        return ErrorCode::PermissionDenied;
    }

    // req 幂等：同一个 reqId 重复提交（双击、超时重试）只建一个节点。
    const std::string reqKey = cm::cache::CacheKey::shareReq(uid, rq.reqId);
    if (rq.reqId != 0) {
        std::string cached;
        if (m_cache.get(reqKey, &cached) == cm::cache::CacheStore::Result::Hit) {
            if (out->decode(cached)) {
                return ErrorCode::Ok;  // 回放上一次的结果，不再建节点
            }
            // 缓存是脏数据：按未命中处理，成功后再覆盖
        }
    }

    // 分享校验与 open 完全一致（状态/过期/提取码 fail-closed）
    ShareMeta meta;
    std::uint32_t wireCode = 0;
    const ErrorCode resolved =
        resolveShare(m_cache, m_mysql, m_redis, rq.shareCode, rq.accessCode,
                     clientKey, &meta, &wireCode);
    if (resolved != ErrorCode::Ok) {
        out->code = wireCode;
        return resolved;
    }

    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        const ErrorCode err = dbPoolError(m_mysql);
        out->code = static_cast<std::uint32_t>(err);
        return err;
    }
    cm::db::FileDao fileDao(*conn);

    // 源节点必须仍有效：分享缓存 5min，文件可能在缓存有效期内被删
    cm::db::UserFileRow node;
    ErrorCode rc = fileDao.findUserFile(meta.userId, meta.ufId, &node);
    if (rc == ErrorCode::NotFound ||
        (rc == ErrorCode::Ok &&
         node.deletedAt != cm::db::FileDao::activeDeletedAt())) {
        conn.reset();
        out->code = static_cast<std::uint32_t>(ErrorCode::NotFound);
        return ErrorCode::NotFound;
    }
    if (rc != ErrorCode::Ok) {
        conn.reset();
        out->code = static_cast<std::uint32_t>(rc);
        return rc;
    }
    if (node.isDir) {
        // 目录转存要展开整棵子树，本期不做（分享只支持单文件）
        conn.reset();
        out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        return ErrorCode::InvalidArgument;
    }

    cm::db::BlobRow blob;
    rc = fileDao.getBlobById(node.fid, &blob);
    if (rc == ErrorCode::NotFound) {
        conn.reset();
        out->code = static_cast<std::uint32_t>(ErrorCode::FileGone);
        return ErrorCode::FileGone;
    }
    if (rc != ErrorCode::Ok) {
        conn.reset();
        out->code = static_cast<std::uint32_t>(rc);
        return rc;
    }

    // 目标目录必须是自己的、未删除的目录；0 = 根目录
    if (rq.parentId != 0) {
        cm::db::UserFileRow parent;
        rc = fileDao.findUserFile(uid, rq.parentId, &parent);
        if (rc == ErrorCode::NotFound) {
            conn.reset();
            out->code = static_cast<std::uint32_t>(ErrorCode::NotFound);
            return ErrorCode::NotFound;
        }
        if (rc != ErrorCode::Ok) {
            conn.reset();
            out->code = static_cast<std::uint32_t>(rc);
            return rc;
        }
        if (parent.deletedAt != cm::db::FileDao::activeDeletedAt() ||
            !parent.isDir) {
            conn.reset();
            out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
            return ErrorCode::InvalidArgument;
        }
    }

    // 转存 = 秒传落库：新增节点 + blob 引用计数 +1，不搬字节。
    // 同名同内容已存在时直接复用（reused=1），引用计数不变。
    std::uint64_t fileId = 0;
    std::uint64_t ufId = 0;
    std::string finalName;
    bool reused = false;
    rc = fileDao.materializeInstantUpload(uid, rq.parentId, blob.md5, blob.size,
                                          node.name, &fileId, &ufId, &finalName,
                                          &reused);
    conn.reset();
    if (rc == ErrorCode::NotFound) {
        // blob 在「命中判定」与「落库」之间被孤儿 GC 回收
        out->code = static_cast<std::uint32_t>(ErrorCode::FileGone);
        return ErrorCode::FileGone;
    }
    if (rc != ErrorCode::Ok) {
        out->code = static_cast<std::uint32_t>(rc);
        return rc;
    }

    out->code = 0;
    out->ufId = ufId;
    out->fileName = finalName;
    out->size = blob.size;
    out->reused = reused ? 1 : 0;

    if (rq.reqId != 0) {
        std::string payload;
        if (out->encode(&payload)) {
            m_cache.put(reqKey, payload,
                        std::chrono::milliseconds(kReqResultTtlMs));
        }
    }
    return ErrorCode::Ok;
}

}  // namespace logic
}  // namespace cm
