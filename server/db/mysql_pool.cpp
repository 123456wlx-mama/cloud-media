#include "mysql_pool.h"

#include <mysql.h>

#include <sstream>

#include "cm/log/logger.h"
#include <utility>

namespace cm {
namespace db {
namespace {

MysqlHandle createPhysical(const MysqlOptions& options,
                           unsigned int connectTimeoutSec,
                           std::string* err) {
    MYSQL* raw = mysql_init(nullptr);
    if (raw == nullptr) {
        if (err != nullptr) {
            *err = "mysql_init failed";
        }
        return MysqlHandle();
    }

    unsigned int connectTimeout = connectTimeoutSec < 1 ? 1 : connectTimeoutSec;
    unsigned int readTimeout = 5;
    unsigned int writeTimeout = 5;
    my_bool reconnect = 0;
    mysql_options(raw, MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeout);
    mysql_options(raw, MYSQL_OPT_READ_TIMEOUT, &readTimeout);
    mysql_options(raw, MYSQL_OPT_WRITE_TIMEOUT, &writeTimeout);
    mysql_options(raw, MYSQL_OPT_RECONNECT, &reconnect);
    mysql_options(raw, MYSQL_SET_CHARSET_NAME, options.charset.c_str());

    if (mysql_real_connect(raw, options.host.c_str(), options.user.c_str(),
                           options.password.c_str(), options.database.c_str(),
                           options.port, nullptr, 0) == nullptr) {
        if (err != nullptr) {
            *err = mysql_error(raw);
        }
        mysql_close(raw);
        return MysqlHandle();
    }

    static const char* kSessionSql[] = {
        "SET NAMES utf8mb4",
        "SET SESSION time_zone = '+00:00'",
        "SET SESSION sql_mode = 'STRICT_TRANS_TABLES,NO_ZERO_DATE,"
        "ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION'",
        "SET SESSION autocommit = 1",
        "SET SESSION innodb_lock_wait_timeout = 5",
        "SET SESSION max_execution_time = 2000"
    };
    const std::size_t sessionCount = sizeof(kSessionSql) / sizeof(kSessionSql[0]);
    for (std::size_t i = 0; i < sessionCount; ++i) {
        if (mysql_query(raw, kSessionSql[i]) != 0) {
            if (err != nullptr) {
                *err = std::string("session init failed: ") + mysql_error(raw);
            }
            mysql_close(raw);
            return MysqlHandle();
        }
    }
    return MysqlHandle(raw);
}

}  // namespace

MysqlOptions::MysqlOptions()
    : host(),
      port(3306),
      user(),
      password(),
      database(),
      charset("utf8mb4"),
      minSize(2),
      maxSize(8),
      idleTimeout(60000),
      acquireTimeout(500),
      maxLifetime(1800000),
      slowQueryThreshold(200) {
}

bool MysqlOptions::validate(std::string* err) const {
    std::string message;
    if (host.empty()) {
        message = "mysql host is empty";
    } else if (user.empty()) {
        message = "mysql user is empty";
    } else if (database.empty()) {
        message = "mysql database is empty";
    } else if (charset.empty()) {
        message = "mysql charset is empty";
    } else if (port == 0) {
        message = "mysql port is invalid";
    } else if (minSize < 1) {
        message = "mysql pool minSize must be >= 1";
    } else if (maxSize < minSize) {
        message = "mysql pool maxSize must be >= minSize";
    } else if (maxSize > 32) {
        message = "mysql pool maxSize must be <= 32";
    } else if (idleTimeout.count() < 0 ||
               acquireTimeout.count() < 0 ||
               maxLifetime.count() < 0 ||
               slowQueryThreshold.count() < 0) {
        message = "mysql timeouts must be non-negative";
    }

    if (!message.empty()) {
        if (err != nullptr) {
            *err = message;
        }
        return false;
    }
    if (err != nullptr) {
        err->clear();
    }
    return true;
}

std::string formatMysqlDescription(const MysqlOptions& options,
                                   const MysqlPoolStats& stats) {
    std::ostringstream out;
    out << "mysql host=" << options.host
        << " port=" << options.port
        << " db=" << options.database
        << " user=" << options.user
        << " pool total=" << stats.total
        << " idle=" << stats.idle
        << " in_use=" << stats.inUse
        << " max=" << options.maxSize
        << " waiters=" << stats.waiters
        << " acquire=" << stats.acquireTotal
        << " timeout=" << stats.acquireTimeoutTotal
        << " create=" << stats.createTotal
        << " create_fail=" << stats.createFailTotal
        << " slow=" << stats.slowQueryTotal;
    return out.str();
}

ErrorCode classifyMysqlError(unsigned int code) {
    switch (code) {
        case 2003:  // CR_CONN_HOST_ERROR
        case 2006:  // CR_SERVER_GONE_ERROR
        case 2013:  // CR_SERVER_LOST
            return ErrorCode::DbUnavailable;
        case 1205:  // ER_LOCK_WAIT_TIMEOUT
        case 1213:  // ER_LOCK_DEADLOCK
            return ErrorCode::DbTxConflict;
        case 1062:  // ER_DUP_ENTRY
            return ErrorCode::AlreadyExists;
        default:
            return ErrorCode::DbQueryFailed;
    }
}

namespace detail {

PoolCore::PoolCore(const MysqlOptions& opts)
    : options(opts),
      total(0),
      inUse(0),
      waiters(0),
      stopping(false),
      consecutiveCreateFails(0),
      circuitOpenUntil(std::chrono::steady_clock::time_point::min()),
      reaperStop(false),
      acquireTotal(0),
      acquireTimeoutTotal(0),
      createTotal(0),
      createFailTotal(0),
      slowQueryTotal(0),
      slowQueryMaxMs(0) {
}

PoolCore::~PoolCore() {
    shutdown();
}

void PoolCore::startReaper() {
    reaper = std::thread(&PoolCore::reaperLoop, this);
}

void PoolCore::giveBack(MysqlHandle handle,
                        std::chrono::steady_clock::time_point createdAt,
                        std::chrono::steady_clock::time_point lastActive,
                        bool broken) {
    MysqlHandle closing;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (inUse > 0) {
            --inUse;
        }

        const bool expired =
            (std::chrono::steady_clock::now() - createdAt) > options.maxLifetime;
        if (broken || stopping || expired || !handle) {
            if (total > 0) {
                --total;
            }
            closing = std::move(handle);
        } else {
            IdleConn slot;
            slot.handle = std::move(handle);
            slot.createdAt = createdAt;
            slot.lastActive = lastActive;
            idle.push_back(std::move(slot));
        }
        cv.notify_all();
    }
}

void PoolCore::shutdown() {
    std::deque<IdleConn> closing;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping) {
            return;
        }
        stopping = true;
        reaperStop.store(true);
        cv.notify_all();
        closing.swap(idle);
        if (total >= closing.size()) {
            total -= closing.size();
        } else {
            total = 0;
        }
    }

    if (reaper.joinable() && reaper.get_id() != std::this_thread::get_id()) {
        reaper.join();
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(5), [this]() { return inUse == 0; });
    }
}

void PoolCore::reaperLoop() {
    while (!reaperStop.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(10), [this]() {
            return stopping || reaperStop.load(std::memory_order_relaxed);
        });
        if (stopping || reaperStop.load(std::memory_order_relaxed)) {
            break;
        }

        const std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now();
        std::deque<IdleConn> expired;
        std::deque<IdleConn>::iterator it = idle.begin();
        while (it != idle.end()) {
            const bool tooOld = (now - it->createdAt) > options.maxLifetime;
            const bool tooIdle = (now - it->lastActive) > options.idleTimeout;
            if (total > options.minSize && (tooOld || tooIdle)) {
                expired.push_back(std::move(*it));
                it = idle.erase(it);
                if (total > 0) {
                    --total;
                }
            } else {
                ++it;
            }
        }
        lock.unlock();
        cv.notify_all();
    }
}

}  // namespace detail

MysqlPool::MysqlPool(const MysqlOptions& options,
                     const std::shared_ptr<detail::PoolCore>& core)
    : m_core(core),
      m_options(options),
      m_lastError(static_cast<int>(ErrorCode::Ok)) {
}

MysqlPool::~MysqlPool() {
    if (m_core) {
        m_core->shutdown();
    }
}

std::shared_ptr<MysqlPool> MysqlPool::create(const MysqlOptions& options,
                                             std::string* err) {
    std::string localError;
    if (!options.validate(&localError)) {
        if (err != nullptr) {
            *err = localError;
        }
        return std::shared_ptr<MysqlPool>();
    }

    std::shared_ptr<detail::PoolCore> core =
        std::make_shared<detail::PoolCore>(options);

    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < options.minSize; ++i) {
        MysqlHandle handle = createPhysical(options, 3, &localError);
        if (!handle) {
            if (err != nullptr) {
                *err = localError;
            }
            return std::shared_ptr<MysqlPool>();
        }
        detail::IdleConn slot;
        slot.handle = std::move(handle);
        slot.createdAt = now;
        slot.lastActive = now;
        core->idle.push_back(std::move(slot));
        ++core->total;
    }

    core->startReaper();
    if (err != nullptr) {
        err->clear();
    }
    return std::shared_ptr<MysqlPool>(new MysqlPool(options, core));
}

std::unique_ptr<MysqlConn> MysqlPool::acquire() {
    return acquire(m_options.acquireTimeout);
}

std::unique_ptr<MysqlConn> MysqlPool::acquire(std::chrono::milliseconds timeout) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(m_core->mutex);

    for (;;) {
        if (m_core->stopping) {
            setLastError(ErrorCode::DbUnavailable);
            return std::unique_ptr<MysqlConn>();
        }
        if (std::chrono::steady_clock::now() < m_core->circuitOpenUntil) {
            setLastError(ErrorCode::DbUnavailable);
            return std::unique_ptr<MysqlConn>();
        }

        if (!m_core->idle.empty()) {
            detail::IdleConn slot = std::move(m_core->idle.front());
            m_core->idle.pop_front();
            ++m_core->inUse;
            lock.unlock();

            const std::chrono::steady_clock::time_point now =
                std::chrono::steady_clock::now();
            bool healthy =
                (now - slot.createdAt) <= m_core->options.maxLifetime;
            if (healthy && now - slot.lastActive > std::chrono::seconds(30)) {
                healthy = (mysql_ping(slot.handle.get()) == 0);
            }

            if (!healthy) {
                slot.handle.reset();
                lock.lock();
                if (m_core->total > 0) {
                    --m_core->total;
                }
                if (m_core->inUse > 0) {
                    --m_core->inUse;
                }
                m_core->cv.notify_all();
                if (std::chrono::steady_clock::now() >= deadline) {
                    setLastError(ErrorCode::DbPoolTimeout);
                    logAcquireTimeout();
                    return std::unique_ptr<MysqlConn>();
                }
                continue;
            }

            m_core->acquireTotal.fetch_add(1, std::memory_order_relaxed);
            return std::unique_ptr<MysqlConn>(
                new MysqlConn(m_core, std::move(slot.handle), slot.createdAt,
                              m_options.slowQueryThreshold));
        }

        if (m_core->total < m_core->options.maxSize) {
            const std::chrono::steady_clock::time_point now =
                std::chrono::steady_clock::now();
            if (timeout.count() <= 0 || now >= deadline) {
                m_core->acquireTimeoutTotal.fetch_add(1, std::memory_order_relaxed);
                setLastError(ErrorCode::DbPoolTimeout);
                logAcquireTimeout();
                return std::unique_ptr<MysqlConn>();
            }

            const long long remainingMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now).count();
            long long remainingSec = (remainingMs + 999) / 1000;
            if (remainingSec < 1) {
                remainingSec = 1;
            }
            unsigned int connectSec = 3;
            if (remainingSec < 3) {
                connectSec = static_cast<unsigned int>(remainingSec);
            }

            ++m_core->total;
            ++m_core->inUse;
            lock.unlock();

            std::string connectError;
            MysqlHandle handle = createPhysical(m_core->options, connectSec, &connectError);
            if (!handle) {
                lock.lock();
                if (m_core->total > 0) {
                    --m_core->total;
                }
                if (m_core->inUse > 0) {
                    --m_core->inUse;
                }
                m_core->createFailTotal.fetch_add(1, std::memory_order_relaxed);
                ++m_core->consecutiveCreateFails;
                if (m_core->consecutiveCreateFails >= 3) {
                    m_core->circuitOpenUntil =
                        std::chrono::steady_clock::now() + std::chrono::seconds(5);
                }
                m_core->cv.notify_all();
                if (std::chrono::steady_clock::now() >= deadline) {
                    setLastError(ErrorCode::DbPoolTimeout);
                    logAcquireTimeout();
                } else {
                    setLastError(ErrorCode::DbUnavailable);
                }
                return std::unique_ptr<MysqlConn>();
            }

            m_core->consecutiveCreateFails = 0;
            m_core->createTotal.fetch_add(1, std::memory_order_relaxed);
            m_core->acquireTotal.fetch_add(1, std::memory_order_relaxed);
            return std::unique_ptr<MysqlConn>(
                new MysqlConn(m_core, std::move(handle),
                              std::chrono::steady_clock::now(),
                              m_options.slowQueryThreshold));
        }

        if (timeout.count() <= 0 ||
            std::chrono::steady_clock::now() >= deadline) {
            m_core->acquireTimeoutTotal.fetch_add(1, std::memory_order_relaxed);
            setLastError(ErrorCode::DbPoolTimeout);
            logAcquireTimeout();
            return std::unique_ptr<MysqlConn>();
        }

        ++m_core->waiters;
        const bool signaled = m_core->cv.wait_until(lock, deadline, [this]() {
            return m_core->stopping || !m_core->idle.empty() ||
                   m_core->total < m_core->options.maxSize;
        });
        --m_core->waiters;

        if (!signaled) {
            m_core->acquireTimeoutTotal.fetch_add(1, std::memory_order_relaxed);
            setLastError(ErrorCode::DbPoolTimeout);
            logAcquireTimeout();
            return std::unique_ptr<MysqlConn>();
        }
    }
}

MysqlPoolStats MysqlPool::stats() const {
    MysqlPoolStats result;
    {
        std::lock_guard<std::mutex> lock(m_core->mutex);
        result.total = m_core->total;
        result.idle = m_core->idle.size();
        result.inUse = m_core->inUse;
        result.waiters = m_core->waiters;
    }
    result.acquireTotal = m_core->acquireTotal.load(std::memory_order_relaxed);
    result.acquireTimeoutTotal =
        m_core->acquireTimeoutTotal.load(std::memory_order_relaxed);
    result.createTotal = m_core->createTotal.load(std::memory_order_relaxed);
    result.createFailTotal =
        m_core->createFailTotal.load(std::memory_order_relaxed);
    result.slowQueryTotal = m_core->slowQueryTotal.load(std::memory_order_relaxed);
    result.slowQueryMaxMs =
        m_core->slowQueryMaxMs.load(std::memory_order_relaxed);
    return result;
}

std::string MysqlPool::describe() const {
    return formatMysqlDescription(m_options, stats());
}

void MysqlPool::shutdown() {
    if (m_core) {
        m_core->shutdown();
    }
}

ErrorCode MysqlPool::lastError() const {
    return static_cast<ErrorCode>(m_lastError.load(std::memory_order_relaxed));
}

void MysqlPool::setLastError(ErrorCode code) {
    m_lastError.store(static_cast<int>(code), std::memory_order_relaxed);
}

void MysqlPool::logAcquireTimeout() const {
    std::ostringstream out;
    out << "mysql pool acquire timeout total=" << m_core->total
        << " idle=" << m_core->idle.size()
        << " in_use=" << m_core->inUse
        << " waiters=" << m_core->waiters
        << " max=" << m_core->options.maxSize;
    Logger::instance().log(LogLevel::Warn, __FILE__, __LINE__, out.str());
}

}  // namespace db
}  // namespace cm
