#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "redis_pool.h"

namespace cm {
namespace cache {

class CacheKey {
public:
    static std::string session(const std::string& token);
    static std::string user(std::uint64_t uid);
    static std::string fileByMd5(const std::string& md5);
    static std::string fileById(std::uint64_t fid);
    static std::string share(const std::string& code);
    static std::string shareMiss(const std::string& code);
    static std::string uploadBitmap(const std::string& taskId);
    static std::string rateLimitIp(const std::string& ip, std::int64_t window);
    static std::string rebuildLock(const std::string& key);
    static std::string downloadTicket(const std::string& token);

    static bool isSafeKey(const std::string& key);

private:
    static std::string join(const std::string& prefix, const std::string& value);
};

// 纯函数：baseMs * (100 + randPercent) / 100，结果最小 1ms。
// randPercent 由调用方保证在 [-jitterPercent, jitterPercent]。
std::int64_t jitterTtlMs(std::int64_t baseMs, int jitterPercent,
                         int randPercent);
std::int64_t randomJitterTtlMs(std::int64_t baseMs, int jitterPercent);

struct CacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t nullHits = 0;
    std::uint64_t loads = 0;
    std::uint64_t loadErrors = 0;
    std::uint64_t setFailures = 0;
    std::uint64_t rebuildFallbacks = 0;
    std::uint64_t errors = 0;

    // hitRate = hits / (hits + misses + nullHits)；分母为 0 时返回 0。
    double hitRate() const;
};

struct CacheStoreOptions {
    std::chrono::milliseconds defaultTtl;
    std::chrono::milliseconds nullTtl;
    int jitterPercent;
    std::chrono::milliseconds lockTtl;
    int lockRetries;
    std::chrono::milliseconds lockRetrySleep;

    CacheStoreOptions();
};

// 旁路缓存：
// - 空值标记防穿透
// - 本地分片锁 + Redis SET NX 锁防击穿
// - TTL ±jitterPercent 抖动防雪崩
class CacheStore {
public:
    enum class Result {
        Hit,
        Miss,
        Error
    };

    enum class LoadStatus {
        Found,
        NotFound,
        Error
    };

    using Loader = std::function<LoadStatus(std::string*)>;

    CacheStore(RedisPool& pool, const CacheStoreOptions& options);
    ~CacheStore();

    CacheStore(const CacheStore&) = delete;
    CacheStore& operator=(const CacheStore&) = delete;

    Result get(const std::string& key, std::string* out);
    bool put(const std::string& key, const std::string& value,
             std::chrono::milliseconds ttl);
    bool invalidate(const std::string& key);

    Result getOrLoad(const std::string& key, const Loader& loader,
                     std::string* out);

    CacheStats stats() const;

    static const char* nullMarker();
    static bool isNullMarker(const std::string& value);

private:
    Result read(RedisConn& conn, const std::string& key,
                std::string* out, bool* nullHit);
    Result loadDirect(const Loader& loader, std::string* out);
    bool write(RedisConn& conn, const std::string& key,
               const std::string& value, std::chrono::milliseconds baseTtl);
    void releaseLock(RedisConn& conn, const std::string& lockKey,
                     const std::string& token);
    std::mutex& lockFor(const std::string& key);

    RedisPool& m_pool;
    CacheStoreOptions m_options;
    std::array<std::mutex, 64> m_localLocks;

    std::atomic<std::uint64_t> m_hits;
    std::atomic<std::uint64_t> m_misses;
    std::atomic<std::uint64_t> m_nullHits;
    std::atomic<std::uint64_t> m_loads;
    std::atomic<std::uint64_t> m_loadErrors;
    std::atomic<std::uint64_t> m_setFailures;
    std::atomic<std::uint64_t> m_rebuildFallbacks;
    std::atomic<std::uint64_t> m_errors;
};

}  // namespace cache
}  // namespace cm
