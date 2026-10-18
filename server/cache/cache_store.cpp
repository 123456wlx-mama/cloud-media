#include "cache_store.h"

#include <cctype>
#include <random>
#include <sstream>
#include <thread>

namespace cm {
namespace cache {
namespace {

const char kReleaseLockScript[] =
    "if redis.call('GET', KEYS[1]) == ARGV[1] then "
    "return redis.call('DEL', KEYS[1]) else return 0 end";

std::string makeToken() {
    static std::atomic<std::uint64_t> sequence(0);
    const std::uint64_t now = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::uint64_t seq = sequence.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream out;
    out << std::hex << now << "-" << seq;
    return out.str();
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

std::string CacheKey::share(const std::string& code) {
    if (code.size() != 9) {
        return std::string();
    }
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(code[i]))) {
            return std::string();
        }
    }
    return "cm:v1:share:" + code;
}

std::string CacheKey::shareMiss(const std::string& code) {
    if (code.size() != 9) {
        return std::string();
    }
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(code[i]))) {
            return std::string();
        }
    }
    return "cm:v1:share:miss:" + code;
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

std::mutex& CacheStore::lockFor(const std::string& key) {
    const std::size_t index =
        std::hash<std::string>()(key) % m_localLocks.size();
    return m_localLocks[index];
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
                       std::chrono::milliseconds baseTtl) {
    const std::int64_t ttlMs =
        randomJitterTtlMs(baseTtl.count(), m_options.jitterPercent);
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
    return write(*conn, key, value, ttl);
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

    std::lock_guard<std::mutex> localLock(lockFor(key));

    nullHit = false;
    result = read(*conn, key, out, &nullHit);
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
            return Result::Hit;
        }
        if (result == Result::Miss && nullHit) {
            m_nullHits.fetch_add(1, std::memory_order_relaxed);
            return Result::Miss;
        }
        if (result == Result::Error) {
            break;
        }
    }

    if (!locked) {
        m_rebuildFallbacks.fetch_add(1, std::memory_order_relaxed);
    }

    if (locked) {
        nullHit = false;
        result = read(*conn, key, out, &nullHit);
        if (result == Result::Hit) {
            m_hits.fetch_add(1, std::memory_order_relaxed);
            releaseLock(*conn, lockKey, token);
            return Result::Hit;
        }
        if (result == Result::Miss && nullHit) {
            m_nullHits.fetch_add(1, std::memory_order_relaxed);
            releaseLock(*conn, lockKey, token);
            return Result::Miss;
        }
        if (result == Result::Error) {
            m_errors.fetch_add(1, std::memory_order_relaxed);
            releaseLock(*conn, lockKey, token);
            conn.reset();
            return loadDirect(loader, out);
        }
    }

    m_misses.fetch_add(1, std::memory_order_relaxed);
    m_loads.fetch_add(1, std::memory_order_relaxed);
    const LoadStatus status = loader(out);
    if (status == LoadStatus::Found) {
        write(*conn, key, *out, m_options.defaultTtl);
    } else if (status == LoadStatus::NotFound) {
        write(*conn, key, nullMarker(), m_options.nullTtl);
    } else {
        m_loadErrors.fetch_add(1, std::memory_order_relaxed);
    }

    if (locked) {
        releaseLock(*conn, lockKey, token);
    }
    if (status == LoadStatus::Found) {
        return Result::Hit;
    }
    return status == LoadStatus::NotFound ? Result::Miss : Result::Error;
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
