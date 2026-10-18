#pragma once

#include <hiredis/hiredis.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "cm/base/error_code.h"

namespace cm {
namespace cache {

struct RedisCloser {
    void operator()(redisContext* ctx) const {
        if (ctx != nullptr) {
            redisFree(ctx);
        }
    }
};

using RedisCtxHandle = std::unique_ptr<redisContext, RedisCloser>;

// redisReply 的 RAII 包装；只拥有顶层回复。
class RedisReply {
public:
    RedisReply();
    explicit RedisReply(redisReply* reply);
    ~RedisReply();

    RedisReply(RedisReply&& other) noexcept;
    RedisReply& operator=(RedisReply&& other) noexcept;

    RedisReply(const RedisReply&) = delete;
    RedisReply& operator=(const RedisReply&) = delete;

    bool valid() const;
    int type() const;
    bool isNil() const;
    bool isError() const;
    long long integer() const;
    std::string str() const;
    std::size_t elements() const;
    const redisReply* element(std::size_t index) const;

    void reset(redisReply* reply = nullptr);

private:
    redisReply* m_reply;
};

struct RedisOptions {
    std::string host;
    int port;
    std::string password;
    int db;
    std::chrono::milliseconds connectTimeout;
    std::chrono::milliseconds commandTimeout;
    std::size_t minSize;
    std::size_t maxSize;
    std::chrono::milliseconds idleTimeout;
    std::chrono::milliseconds acquireTimeout;
    std::chrono::milliseconds maxLifetime;

    RedisOptions();

    bool validate(std::string* err) const;
};

struct RedisPoolStats {
    std::size_t total = 0;
    std::size_t idle = 0;
    std::size_t inUse = 0;
    std::size_t waiters = 0;
    std::uint64_t acquireTotal = 0;
    std::uint64_t acquireTimeoutTotal = 0;
    std::uint64_t createTotal = 0;
    std::uint64_t createFailTotal = 0;
    std::uint64_t reconnectTotal = 0;
    std::uint64_t commandErrors = 0;
};

// 只输出 host/port/db 与池计数；绝不读取 password。
std::string formatRedisDescription(const RedisOptions& options,
                                   const RedisPoolStats& stats);

enum class RedisLookup {
    Hit,
    Miss,
    Error
};

namespace detail {
struct RedisCore;
}

// 单条 Redis 连接；同一时刻只允许一个线程使用（由 RedisPool 借出保证）。
class RedisConn {
public:
    ~RedisConn();

    RedisConn(const RedisConn&) = delete;
    RedisConn& operator=(const RedisConn&) = delete;

    bool valid() const;
    bool ping();
    bool reconnect();

    RedisLookup get(const std::string& key, std::string* value);
    bool set(const std::string& key, const std::string& value);
    bool setPx(const std::string& key, const std::string& value,
               std::chrono::milliseconds ttl);
    bool setNxPx(const std::string& key, const std::string& value,
                 std::chrono::milliseconds ttl);
    bool expire(const std::string& key, std::chrono::seconds ttl);
    bool hset(const std::string& key, const std::string& field,
              const std::string& value);
    RedisLookup hget(const std::string& key, const std::string& field,
                     std::string* value);
    bool incr(const std::string& key, long long* value);
    bool del(const std::string& key);

    // script 必须是常量且不能包含 %（redisCommand 使用 printf 风格格式串）。
    bool eval(const std::string& script, const std::string& key,
              const std::string& arg, RedisReply* reply);

    ErrorCode lastError() const { return m_lastError; }
    const std::string& lastErrorMessage() const { return m_lastErrorMsg; }

private:
    friend class RedisPool;
    friend struct detail::RedisCore;

    RedisConn(const std::shared_ptr<detail::RedisCore>& core,
              RedisCtxHandle ctx,
              std::chrono::steady_clock::time_point createdAt);

    void setError(ErrorCode code, const std::string& message);
    void deleteWrongTypeKey(const std::string& key);
    void markBroken();

    std::shared_ptr<detail::RedisCore> m_core;
    RedisCtxHandle m_ctx;
    std::chrono::steady_clock::time_point m_createdAt;
    bool m_broken;
    ErrorCode m_lastError;
    std::string m_lastErrorMsg;
};

namespace detail {

struct IdleRedis {
    RedisCtxHandle ctx;
    std::chrono::steady_clock::time_point createdAt;
    std::chrono::steady_clock::time_point lastActive;
};

struct RedisCore {
    explicit RedisCore(const RedisOptions& options);
    ~RedisCore();

    RedisCore(const RedisCore&) = delete;
    RedisCore& operator=(const RedisCore&) = delete;

    void startReaper();
    void giveBack(RedisCtxHandle ctx,
                  std::chrono::steady_clock::time_point createdAt,
                  std::chrono::steady_clock::time_point lastActive,
                  bool broken);
    void shutdown();

    RedisOptions options;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<IdleRedis> idle;
    std::size_t total;
    std::size_t inUse;
    std::size_t waiters;
    bool stopping;
    std::atomic<int> consecutiveCreateFails;
    std::chrono::steady_clock::time_point circuitOpenUntil;
    std::thread reaper;
    std::atomic<bool> reaperStop;

    std::atomic<std::uint64_t> acquireTotal;
    std::atomic<std::uint64_t> acquireTimeoutTotal;
    std::atomic<std::uint64_t> createTotal;
    std::atomic<std::uint64_t> createFailTotal;
    std::atomic<std::uint64_t> reconnectTotal;
    std::atomic<std::uint64_t> commandErrors;

private:
    void reaperLoop();
};

}  // namespace detail

class RedisPool {
public:
    static std::shared_ptr<RedisPool> create(const RedisOptions& options,
                                             std::string* err);

    ~RedisPool();

    RedisPool(const RedisPool&) = delete;
    RedisPool& operator=(const RedisPool&) = delete;

    std::unique_ptr<RedisConn> acquire();
    std::unique_ptr<RedisConn> acquire(std::chrono::milliseconds timeout);

    RedisPoolStats stats() const;
    std::string describe() const;
    void shutdown();

    ErrorCode lastError() const;

private:
    RedisPool(const RedisOptions& options,
              const std::shared_ptr<detail::RedisCore>& core);

    void logAcquireTimeout() const;
    void setLastError(ErrorCode code);

    std::shared_ptr<detail::RedisCore> m_core;
    RedisOptions m_options;
    std::atomic<int> m_lastError;
};

}  // namespace cache
}  // namespace cm
