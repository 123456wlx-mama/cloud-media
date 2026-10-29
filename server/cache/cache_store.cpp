#include "cache_store.h"

#include <cctype>
#include <random>
#include <set>
#include <sstream>
#include <thread>

namespace cm {
namespace cache {
namespace {

const char kReleaseLockScript[] =
    "if redis.call('GET', KEYS[1]) == ARGV[1] then "
    "return redis.call('DEL', KEYS[1]) else return 0 end";

std::string makeToken() {
    // 128 位随机：token 会写进 Redis 参与锁归属比对（Lua 里 GET == ARGV[1] 才 DEL），
    // 可预测的 token 等于把"释放别人锁"的能力交出去。时间戳+序号在这里没有优势。
    static const char kHex[] = "0123456789abcdef";
    std::random_device device;
    std::string token;
    token.reserve(32);
    for (int i = 0; i < 16; ++i) {
        const unsigned int byte = device() & 0xffu;
        token.push_back(kHex[byte >> 4]);
        token.push_back(kHex[byte & 0x0fu]);
    }
    return token;
}

}  // namespace

std::string CacheKey::join(const std::string& prefix,
                           const std::string& value) {
    if (!isSafeKey(value)) {
        return std::string();
    }
    return prefix + value;
}

bool CacheKey::isSafeKey(const std::string& key) {
    if (key.empty() || key.size() > 512) {
        return false;
    }
    for (std::size_t i = 0; i < key.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(key[i]);
        if (c <= 0x20 || c == 0x7f) {
            return false;
        }
    }
    return true;
}

std::string CacheKey::session(const std::string& token) {
    return join("cm:v1:sess:", token);
}

std::string CacheKey::user(std::uint64_t uid) {
    return "cm:v1:usr:" + std::to_string(uid);
}

std::string CacheKey::fileByMd5(const std::string& md5) {
    if (md5.size() != 32) {
        return std::string();
    }
    for (std::size_t i = 0; i < md5.size(); ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(md5[i]))) {
            return std::string();
        }
    }
    return "cm:v1:file:md5:" + md5;
}

std::string CacheKey::fileById(std::uint64_t fid) {
    return "cm:v1:file:id:" + std::to_string(fid);
}

namespace {

// 31 字符表：2-9 + A-Z，去掉易混淆的 I/L/O（0/1 也不在数字段）。
bool isShareCodeChar(unsigned char c) {
    if (c >= '2' && c <= '9') {
        return true;
    }
    return c >= 'A' && c <= 'Z' && c != 'I' && c != 'L' && c != 'O';
}

bool isValidShareCode(const std::string& code) {
    if (code.size() != 6) {
        return false;
    }
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (!isShareCodeChar(static_cast<unsigned char>(code[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string CacheKey::share(const std::string& code) {
    if (!isValidShareCode(code)) {
        return std::string();
    }
    return "cm:v1:share:" + code;
}

std::string CacheKey::shareMiss(const std::string& code) {
    if (!isValidShareCode(code)) {
        return std::string();
    }
    return "cm:v1:share:miss:" + code;
}

std::string CacheKey::uploadReq(std::uint64_t uid, std::uint32_t reqId) {
    return join("cm:v1:up:req:" + std::to_string(uid) + ":",
                std::to_string(reqId));
}

std::string CacheKey::shareReq(std::uint64_t uid, std::uint32_t reqId) {
    return join("cm:v1:share:req:" + std::to_string(uid) + ":",
                std::to_string(reqId));
}

std::string CacheKey::uploadBitmap(const std::string& taskId) {
    return join("cm:v1:up:bitmap:", taskId);
}

std::string CacheKey::rateLimitIp(const std::string& ip, std::int64_t window) {
    if (!isSafeKey(ip)) {
        return std::string();
    }
    return "cm:v1:rl:ip:" + ip + ":" + std::to_string(window);
}

std::string CacheKey::rebuildLock(const std::string& key) {
    return join("cm:v1:lock:rebuild:", key);
}

std::string CacheKey::downloadTicket(const std::string& token) {
    return join("cm:v1:dl:ticket:", token);
}

std::string CacheKey::downloadDone(std::uint64_t uid,
                                   const std::string& ticket) {
    return join("cm:v1:dl:done:" + std::to_string(uid) + ":", ticket);
}

std::string CacheKey::shareTicket(const std::string& token) {
    return join("cm:v1:share:ticket:", token);
}

std::string CacheKey::shareFail(const std::string& code,
                                const std::string& scope) {
    if (!isValidShareCode(code) || !isSafeKey(scope)) {
        return std::string();
    }
    return "cm:v1:share:fail:" + code + ":" + scope;
}

std::int64_t jitterTtlMs(std::int64_t baseMs, int jitterPercent,
                         int randPercent) {
    if (baseMs <= 0) {
        return 0;
    }
    if (jitterPercent < 0) {
        jitterPercent = 0;
    }
    if (jitterPercent > 100) {
        jitterPercent = 100;
    }
    if (randPercent < -jitterPercent) {
        randPercent = -jitterPercent;
    }
    if (randPercent > jitterPercent) {
        randPercent = jitterPercent;
    }
    const std::int64_t result = baseMs * (100 + randPercent) / 100;
    return result < 1 ? 1 : result;
}

std::int64_t randomJitterTtlMs(std::int64_t baseMs, int jitterPercent) {
    if (baseMs <= 0) {
        return 0;
    }
    if (jitterPercent <= 0) {
        return baseMs;
    }
    if (jitterPercent > 100) {
        jitterPercent = 100;
    }
    static thread_local std::mt19937 engine(
        static_cast<std::mt19937::result_type>(
            std::chrono::steady_clock::now().time_since_epoch().count()) ^
        static_cast<std::mt19937::result_type>(std::random_device()()));
    std::uniform_int_distribution<int> dist(-jitterPercent, jitterPercent);
    return jitterTtlMs(baseMs, jitterPercent, dist(engine));
}

double CacheStats::hitRate() const {
    const std::uint64_t denominator = hits + misses + nullHits;
    if (denominator == 0) {
        return 0.0;
    }
    return static_cast<double>(hits) / static_cast<double>(denominator);
}

CacheStoreOptions::CacheStoreOptions()
    : defaultTtl(600000),
      nullTtl(60000),
      jitterPercent(10),
      lockTtl(3000),
      lockRetries(3),
      lockRetrySleep(20) {
}

CacheStore::CacheStore(RedisPool& pool, const CacheStoreOptions& options)
    : m_pool(pool),
      m_options(options),
      m_hits(0),
      m_misses(0),
      m_nullHits(0),
      m_loads(0),
      m_loadErrors(0),
      m_setFailures(0),
      m_rebuildFallbacks(0),
      m_errors(0) {
}

CacheStore::~CacheStore() {
}

const char* CacheStore::nullMarker() {
    return "__cm_null__";
}

bool CacheStore::isNullMarker(const std::string& value) {
    return value == nullMarker();
}

std::shared_ptr<CacheStore::RebuildSlot> CacheStore::findOrCreateSlot(
    const std::string& key, bool* isLeader) {
    std::lock_guard<std::mutex> guard(m_slotMutex);
    std::unordered_map<std::string, std::shared_ptr<RebuildSlot>>::iterator it =
        m_slots.find(key);
    if (it != m_slots.end()) {
        *isLeader = false;
        return it->second;
    }
    std::shared_ptr<RebuildSlot> slot = std::make_shared<RebuildSlot>();
    m_slots.insert(std::make_pair(key, slot));
    *isLeader = true;
    return slot;
}

void CacheStore::publishSlot(const std::string& key, Result result,
                             const std::string& value) {
    std::shared_ptr<RebuildSlot> slot;
    {
        std::lock_guard<std::mutex> guard(m_slotMutex);
        std::unordered_map<std::string, std::shared_ptr<RebuildSlot>>::iterator it =
            m_slots.find(key);
        if (it == m_slots.end()) {
            return;
        }
        slot = it->second;
        m_slots.erase(it);
    }
    {
        std::lock_guard<std::mutex> guard(slot->mutex);
        slot->done = true;
        slot->result = result;
        slot->value = value;
    }
    slot->cv.notify_all();
}

CacheStore::Result CacheStore::read(RedisConn& conn, const std::string& key,
                                    std::string* out, bool* nullHit) {
    std::string value;
    const RedisLookup lookup = conn.get(key, &value);
    if (lookup == RedisLookup::Hit) {
        if (isNullMarker(value)) {
            if (nullHit != nullptr) {
                *nullHit = true;
            }
            if (out != nullptr) {
                out->clear();
            }
            return Result::Miss;
        }
        if (nullHit != nullptr) {
            *nullHit = false;
        }
        if (out != nullptr) {
            *out = value;
        }
        return Result::Hit;
    }
    if (lookup == RedisLookup::Miss) {
        if (nullHit != nullptr) {
            *nullHit = false;
        }
        return Result::Miss;
    }
    return Result::Error;
}

CacheStore::Result CacheStore::loadDirect(const Loader& loader,
                                          std::string* out) {
    m_loads.fetch_add(1, std::memory_order_relaxed);
    const LoadStatus status = loader(out);
    if (status == LoadStatus::Error) {
        m_loadErrors.fetch_add(1, std::memory_order_relaxed);
        return Result::Error;
    }
    return status == LoadStatus::Found ? Result::Hit : Result::Miss;
}

bool CacheStore::write(RedisConn& conn, const std::string& key,
                       const std::string& value,
                       std::chrono::milliseconds baseTtl, bool jitter) {
    const std::int64_t ttlMs = jitter
        ? randomJitterTtlMs(baseTtl.count(), m_options.jitterPercent)
        : baseTtl.count();
    if (ttlMs <= 0) {
        return false;
    }
    if (!conn.setPx(key, value, std::chrono::milliseconds(ttlMs))) {
        m_setFailures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void CacheStore::releaseLock(RedisConn& conn, const std::string& lockKey,
                             const std::string& token) {
    conn.eval(kReleaseLockScript, lockKey, token, nullptr);
}

CacheStore::Result CacheStore::get(const std::string& key,
                                   std::string* out) {
    if (!CacheKey::isSafeKey(key) || out == nullptr) {
        return Result::Error;
    }
    std::unique_ptr<RedisConn> conn = m_pool.acquire();
    if (!conn) {
        m_errors.fetch_add(1, std::memory_order_relaxed);
        return Result::Error;
    }

    bool nullHit = false;
    const Result result = read(*conn, key, out, &nullHit);
    if (result == Result::Hit) {
        m_hits.fetch_add(1, std::memory_order_relaxed);
    } else if (result == Result::Miss && nullHit) {
        m_nullHits.fetch_add(1, std::memory_order_relaxed);
    } else if (result == Result::Miss) {
        m_misses.fetch_add(1, std::memory_order_relaxed);
    } else {
        m_errors.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

bool CacheStore::put(const std::string& key, const std::string& value,
                     std::chrono::milliseconds ttl) {
    if (!CacheKey::isSafeKey(key) || ttl.count() <= 0) {
        return false;
    }
    std::unique_ptr<RedisConn> conn = m_pool.acquire();
    if (!conn) {
        m_errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return write(*conn, key, value, ttl, true);
}

CacheStore::LockResult CacheStore::tryLock(const std::string& key,
                                             const std::string& token,
                                             std::chrono::milliseconds ttl) {
    if (!CacheKey::isSafeKey(key) || ttl.count() <= 0) {
        return LockResult::Error;
    }
    std::unique_ptr<RedisConn> conn = m_pool.acquire();
    if (!conn) {
        m_errors.fetch_add(1, std::memory_order_relaxed);
        return LockResult::Error;
    }
    if (conn->setNxPx(key, token, ttl)) {
        return LockResult::Acquired;
    }
    // 空回复（键已存在）不设置 lastError；连接/命令错误会置 RedisUnavailable。
    if (conn->lastError() == ErrorCode::Ok) {
        return LockResult::Busy;
    }
    m_errors.fetch_add(1, std::memory_order_relaxed);
    return LockResult::Error;
}

bool CacheStore::invalidate(const std::string& key) {
    if (!CacheKey::isSafeKey(key)) {
        return false;
    }
    std::unique_ptr<RedisConn> conn = m_pool.acquire();
    if (!conn) {
        m_errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!conn->del(key)) {
        m_errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

CacheStore::Result CacheStore::getOrLoad(const std::string& key,
                                         const Loader& loader,
                                         std::string* out) {
    if (!CacheKey::isSafeKey(key) || !loader || out == nullptr) {
        return Result::Error;
    }

    // 同线程重入（loader 内部又请求同一个 key）：直接回源，避免等待自己造成死锁
    static thread_local std::set<std::string> loadingKeys;
    if (loadingKeys.find(key) != loadingKeys.end()) {
        return loadDirect(loader, out);
    }

    std::unique_ptr<RedisConn> conn = m_pool.acquire();
    if (!conn) {
        m_errors.fetch_add(1, std::memory_order_relaxed);
        return loadDirect(loader, out);
    }

    bool nullHit = false;
    Result result = read(*conn, key, out, &nullHit);
    if (result == Result::Hit) {
        m_hits.fetch_add(1, std::memory_order_relaxed);
        return Result::Hit;
    }
    if (result == Result::Miss && nullHit) {
        m_nullHits.fetch_add(1, std::memory_order_relaxed);
        return Result::Miss;
    }
    if (result == Result::Error) {
        m_errors.fetch_add(1, std::memory_order_relaxed);
        conn.reset();
        return loadDirect(loader, out);
    }

    // 单飞：同一 key 的并发请求只允许一个 leader 回源，其余最多等 200ms
    bool isLeader = false;
    std::shared_ptr<RebuildSlot> slot = findOrCreateSlot(key, &isLeader);
    if (!isLeader) {
        std::unique_lock<std::mutex> waitLock(slot->mutex);
        if (!slot->cv.wait_for(waitLock, std::chrono::milliseconds(200),
                               [&slot]() { return slot->done; })) {
            // 等待超时：不等了，直接回源（此刻不持有任何进程级锁）
            m_rebuildFallbacks.fetch_add(1, std::memory_order_relaxed);
            return loadDirect(loader, out);
        }
        if (slot->result == Result::Hit) {
            *out = slot->value;
            return Result::Hit;
        }
        if (slot->result == Result::Miss) {
            return Result::Miss;
        }
        // leader 失败：自己回源兜底，避免把故障扩散给所有等待者
        return loadDirect(loader, out);
    }

    // leader：登记重入标记 + 保证任何返回路径都会唤醒等待者
    loadingKeys.insert(key);
    Result finalResult = Result::Error;
    std::string finalValue;
    struct LeaderGuard {
        CacheStore* self;
        const std::string& key;
        std::set<std::string>& loading;
        Result& result;
        std::string& value;
        ~LeaderGuard() {
            self->publishSlot(key, result, value);
            loading.erase(key);
        }
    } leaderGuard = {this, key, loadingKeys, finalResult, finalValue};

    const std::string token = makeToken();
    const std::string lockKey = CacheKey::rebuildLock(key);
    bool locked = false;
    for (int i = 0; i < m_options.lockRetries; ++i) {
        if (conn->setNxPx(lockKey, token, m_options.lockTtl)) {
            locked = true;
            break;
        }
        if (!conn->valid()) {
            break;
        }
        std::this_thread::sleep_for(m_options.lockRetrySleep);

        nullHit = false;
        result = read(*conn, key, out, &nullHit);
        if (result == Result::Hit) {
            m_hits.fetch_add(1, std::memory_order_relaxed);
            finalResult = Result::Hit;
            finalValue = *out;
            return Result::Hit;
        }
        if (result == Result::Miss && nullHit) {
            m_nullHits.fetch_add(1, std::memory_order_relaxed);
            finalResult = Result::Miss;
            return Result::Miss;
        }
        if (result == Result::Error) {
            break;
        }
    }

    if (!locked) {
        m_rebuildFallbacks.fetch_add(1, std::memory_order_relaxed);
    }

    // Redis 锁 RAII：异常/提前返回都不留下残留锁
    struct RedisLockGuard {
        CacheStore* self;
        RedisConn* conn;
        std::string lockKey;
        std::string token;
        bool active;
        ~RedisLockGuard() {
            if (active) {
                self->releaseLock(*conn, lockKey, token);
            }
        }
    } redisLock = {this, conn.get(), lockKey, token, locked};

    if (locked) {
        nullHit = false;
        result = read(*conn, key, out, &nullHit);
        if (result == Result::Hit) {
            m_hits.fetch_add(1, std::memory_order_relaxed);
            finalResult = Result::Hit;
            finalValue = *out;
            return Result::Hit;
        }
        if (result == Result::Miss && nullHit) {
            m_nullHits.fetch_add(1, std::memory_order_relaxed);
            finalResult = Result::Miss;
            return Result::Miss;
        }
        if (result == Result::Error) {
            m_errors.fetch_add(1, std::memory_order_relaxed);
            return loadDirect(loader, out);  // 锁由 redisLock 释放
        }
    }

    m_misses.fetch_add(1, std::memory_order_relaxed);
    m_loads.fetch_add(1, std::memory_order_relaxed);
    const LoadStatus status = loader(out);
    if (status == LoadStatus::Found) {
        write(*conn, key, *out, m_options.defaultTtl, true);
        finalResult = Result::Hit;
        finalValue = *out;
        return Result::Hit;
    }
    if (status == LoadStatus::NotFound) {
        write(*conn, key, nullMarker(), m_options.nullTtl, false);
        finalResult = Result::Miss;
        return Result::Miss;
    }
    m_loadErrors.fetch_add(1, std::memory_order_relaxed);
    return Result::Error;
}

CacheStats CacheStore::stats() const {
    CacheStats result;
    result.hits = m_hits.load(std::memory_order_relaxed);
    result.misses = m_misses.load(std::memory_order_relaxed);
    result.nullHits = m_nullHits.load(std::memory_order_relaxed);
    result.loads = m_loads.load(std::memory_order_relaxed);
    result.loadErrors = m_loadErrors.load(std::memory_order_relaxed);
    result.setFailures = m_setFailures.load(std::memory_order_relaxed);
    result.rebuildFallbacks =
        m_rebuildFallbacks.load(std::memory_order_relaxed);
    result.errors = m_errors.load(std::memory_order_relaxed);
    return result;
}

}  // namespace cache
}  // namespace cm
