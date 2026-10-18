#pragma once

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
#include "mysql_conn.h"

namespace cm {
namespace db {

struct MysqlOptions {
    std::string host;
    unsigned short port;
    std::string user;
    std::string password;
    std::string database;
    std::string charset;
    std::size_t minSize;
    std::size_t maxSize;
    std::chrono::milliseconds idleTimeout;
    std::chrono::milliseconds acquireTimeout;
    std::chrono::milliseconds maxLifetime;
    std::chrono::milliseconds slowQueryThreshold;

    MysqlOptions();

    bool validate(std::string* err) const;
};

struct MysqlPoolStats {
    std::size_t total = 0;
    std::size_t idle = 0;
    std::size_t inUse = 0;
    std::size_t waiters = 0;
    std::uint64_t acquireTotal = 0;
    std::uint64_t acquireTimeoutTotal = 0;
    std::uint64_t createTotal = 0;
    std::uint64_t createFailTotal = 0;
    std::uint64_t slowQueryTotal = 0;
    std::uint64_t slowQueryMaxMs = 0;
};

// 只输出 host/port/db/user 与池计数；绝不读取 password。
std::string formatMysqlDescription(const MysqlOptions& options,
                                   const MysqlPoolStats& stats);

ErrorCode classifyMysqlError(unsigned int code);

namespace detail {

struct IdleConn {
    MysqlHandle handle;
    std::chrono::steady_clock::time_point createdAt;
    std::chrono::steady_clock::time_point lastActive;
};

// 连接池共享状态。MysqlConn 持有 shared_ptr，保证归还时它仍然存活。
struct PoolCore {
    explicit PoolCore(const MysqlOptions& options);
    ~PoolCore();

    PoolCore(const PoolCore&) = delete;
    PoolCore& operator=(const PoolCore&) = delete;

    void startReaper();
    void giveBack(MysqlHandle handle,
                  std::chrono::steady_clock::time_point createdAt,
                  std::chrono::steady_clock::time_point lastActive,
                  bool broken);
    void shutdown();

    MysqlOptions options;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<IdleConn> idle;
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
    std::atomic<std::uint64_t> slowQueryTotal;
    std::atomic<std::uint64_t> slowQueryMaxMs;

private:
    void reaperLoop();
};

}  // namespace detail

class MysqlPool {
public:
    static std::shared_ptr<MysqlPool> create(const MysqlOptions& options,
                                             std::string* err);

    ~MysqlPool();

    MysqlPool(const MysqlPool&) = delete;
    MysqlPool& operator=(const MysqlPool&) = delete;

    std::unique_ptr<MysqlConn> acquire();
    std::unique_ptr<MysqlConn> acquire(std::chrono::milliseconds timeout);

    MysqlPoolStats stats() const;
    std::string describe() const;
    void shutdown();

    ErrorCode lastError() const;

private:
    MysqlPool(const MysqlOptions& options,
              const std::shared_ptr<detail::PoolCore>& core);

    void logAcquireTimeout() const;
    void setLastError(ErrorCode code);

    std::shared_ptr<detail::PoolCore> m_core;
    MysqlOptions m_options;
    std::atomic<int> m_lastError;
};

}  // namespace db
}  // namespace cm
