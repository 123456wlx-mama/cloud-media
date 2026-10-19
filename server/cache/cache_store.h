#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

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
    static std::string uploadReq(std::uint64_t uid, std::uint32_t reqId);
    static std::string shareReq(std::uint64_t uid, std::uint32_t reqId);
    static std::string uploadBitmap(const std::string& taskId);
    static std::string rateLimitIp(const std::string& ip, std::int64_t window);
    static std::string rebuildLock(const std::string& key);
    static std::string downloadTicket(const std::string& token);
    static std::string downloadDone(std::uint64_t uid, const std::string& ticket);

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
// - 进程内「按 key 单飞」+ Redis SET NX 锁防击穿（等待上限 200ms，超时回源，不阻塞 loader）
// - TTL ±jitterPercent 抖动防雪崩
// 注意：loader 绝不在任何进程级锁内执行，且同线程重入同 key 会直接回源（防自锁）。
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

    // 短锁结果：Busy 表示别的请求持锁（业务层回 1006），
    // Error 表示 Redis 不可用（调用方按 fail-open 继续，不阻塞主流程）。
    enum class LockResult {
        Acquired,
        Busy,
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

    // SET key token NX PX ttl。只用于幂等短锁，不参与 Cache-Aside 数据。
    LockResult tryLock(const std::string& key, const std::string& token,
                       std::chrono::milliseconds ttl);

    CacheStats stats() const;

    static const char* nullMarker();
    static bool isNullMarker(const std::string& value);

private:
    // 单飞槽位：每个正在回源的 key 一个，leader 完成后唤醒所有等待者
    struct RebuildSlot {
        std::mutex mutex;
        std::condition_variable cv;
        bool done;
        Result result;
        std::string value;

        RebuildSlot() : done(false), result(Result::Error) {}
    };

    Result read(RedisConn& conn, const std::string& key,
                std::string* out, bool* nullHit);
    Result loadDirect(const Loader& loader, std::string* out);
    bool write(RedisConn& conn, const std::string& key,
               const std::string& value, std::chrono::milliseconds baseTtl,
               bool jitter);
    void releaseLock(RedisConn& conn, const std::string& lockKey,
                     const std::string& token);

    std::shared_ptr<RebuildSlot> findOrCreateSlot(const std::string& key,
                                                  bool* isLeader);
    void publishSlot(const std::string& key, Result result,
                     const std::string& value);

    RedisPool& m_pool;
    CacheStoreOptions m_options;

    std::mutex m_slotMutex;
    std::unordered_map<std::string, std::shared_ptr<RebuildSlot>> m_slots;

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
