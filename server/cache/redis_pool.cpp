#include "redis_pool.h"

#include <cstdarg>
#include <sstream>
#include <utility>

// hiredis 0.13（Ubuntu 16.04 apt 版本）的 redisCommand 返回 void*，1.x 起才改为 redisReply*。
// 这里统一包一层做类型收窄，避免每个调用点各写一遍 static_cast。
#if !defined(HIREDIS_MAJOR) || HIREDIS_MAJOR < 1
namespace {
redisReply* cmRedisCommand(redisContext* ctx, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    void* raw = redisvCommand(ctx, fmt, ap);
    va_end(ap);
    return static_cast<redisReply*>(raw);
}
}  // namespace
#define redisCommand cmRedisCommand
#endif

namespace cm {
namespace cache {
namespace {

RedisCtxHandle createPhysical(const RedisOptions& options, std::string* err) {
    struct timeval connectTv;
    connectTv.tv_sec = static_cast<long>(options.connectTimeout.count() / 1000);
    connectTv.tv_usec = static_cast<long>(
        (options.connectTimeout.count() % 1000) * 1000);

    redisContext* ctx = redisConnectWithTimeout(options.host.c_str(),
                                                options.port, connectTv);
    if (ctx == nullptr) {
        if (err != nullptr) {
            *err = "redisConnectWithTimeout returned nullptr";
        }
        return RedisCtxHandle();
    }
    if (ctx->err != 0) {
        if (err != nullptr) {
            *err = ctx->errstr;
        }
        redisFree(ctx);
        return RedisCtxHandle();
    }

    struct timeval commandTv;
    commandTv.tv_sec = static_cast<long>(options.commandTimeout.count() / 1000);
    commandTv.tv_usec = static_cast<long>(
        (options.commandTimeout.count() % 1000) * 1000);
    redisSetTimeout(ctx, commandTv);

    if (!options.password.empty()) {
        redisReply* reply = redisCommand(ctx, "AUTH %b",
                                         options.password.data(),
                                         options.password.size());
        if (reply == nullptr || ctx->err != 0) {
            if (err != nullptr) {
                *err = ctx->errstr;
            }
            if (reply != nullptr) {
                freeReplyObject(reply);
            }
            redisFree(ctx);
            return RedisCtxHandle();
        }
        if (reply->type == REDIS_REPLY_ERROR) {
            if (err != nullptr) {
                *err = std::string(reply->str, reply->len);
            }
            freeReplyObject(reply);
            redisFree(ctx);
            return RedisCtxHandle();
        }
        freeReplyObject(reply);
    }

    if (options.db != 0) {
        redisReply* reply = redisCommand(ctx, "SELECT %d", options.db);
        if (reply == nullptr || ctx->err != 0) {
            if (err != nullptr) {
                *err = ctx->errstr;
            }
            if (reply != nullptr) {
                freeReplyObject(reply);
            }
            redisFree(ctx);
            return RedisCtxHandle();
        }
        if (reply->type == REDIS_REPLY_ERROR) {
            if (err != nullptr) {
                *err = std::string(reply->str, reply->len);
            }
            freeReplyObject(reply);
            redisFree(ctx);
            return RedisCtxHandle();
        }
        freeReplyObject(reply);
    }

    if (err != nullptr) {
        err->clear();
    }
    return RedisCtxHandle(ctx);
}

}  // namespace

RedisReply::RedisReply() : m_reply(nullptr) {
}

RedisReply::RedisReply(redisReply* reply) : m_reply(reply) {
}

RedisReply::~RedisReply() {
    reset();
}

RedisReply::RedisReply(RedisReply&& other) noexcept : m_reply(other.m_reply) {
    other.m_reply = nullptr;
}

RedisReply& RedisReply::operator=(RedisReply&& other) noexcept {
    if (this != &other) {
        reset();
        m_reply = other.m_reply;
        other.m_reply = nullptr;
    }
    return *this;
}

bool RedisReply::valid() const {
    return m_reply != nullptr;
}

int RedisReply::type() const {
    return m_reply != nullptr ? m_reply->type : -1;
}

bool RedisReply::isNil() const {
    return m_reply != nullptr && m_reply->type == REDIS_REPLY_NIL;
}

bool RedisReply::isError() const {
    return m_reply != nullptr && m_reply->type == REDIS_REPLY_ERROR;
}

long long RedisReply::integer() const {
    return m_reply != nullptr ? m_reply->integer : 0;
}

std::string RedisReply::str() const {
    if (m_reply == nullptr || m_reply->str == nullptr) {
        return std::string();
    }
    return std::string(m_reply->str, m_reply->len);
}

std::size_t RedisReply::elements() const {
    return m_reply != nullptr ? m_reply->elements : 0;
}

const redisReply* RedisReply::element(std::size_t index) const {
    if (m_reply == nullptr || m_reply->type != REDIS_REPLY_ARRAY ||
        index >= m_reply->elements) {
        return nullptr;
    }
    return m_reply->element[index];
}

void RedisReply::reset(redisReply* reply) {
    if (m_reply != nullptr && m_reply != reply) {
        freeReplyObject(m_reply);
    }
    m_reply = reply;
}

RedisOptions::RedisOptions()
    : host("127.0.0.1"),
      port(6379),
      password(),
      db(0),
      connectTimeout(200),
      commandTimeout(300),
      minSize(2),
      maxSize(8),
      idleTimeout(60000),
      acquireTimeout(200),
      maxLifetime(1800000) {
}

bool RedisOptions::validate(std::string* err) const {
    std::string message;
    if (host.empty()) {
        message = "redis host is empty";
    } else if (port <= 0) {
        message = "redis port is invalid";
    } else if (db < 0) {
        message = "redis db is invalid";
    } else if (minSize < 1) {
        message = "redis pool minSize must be >= 1";
    } else if (maxSize < minSize) {
        message = "redis pool maxSize must be >= minSize";
    } else if (maxSize > 32) {
        message = "redis pool maxSize must be <= 32";
    } else if (connectTimeout.count() < 0 ||
               commandTimeout.count() < 0 ||
               idleTimeout.count() < 0 ||
               acquireTimeout.count() < 0 ||
               maxLifetime.count() < 0) {
        message = "redis timeouts must be non-negative";
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

std::string formatRedisDescription(const RedisOptions& options,
                                   const RedisPoolStats& stats) {
    std::ostringstream out;
    out << "redis host=" << options.host
        << " port=" << options.port
        << " db=" << options.db
        << " pool total=" << stats.total
        << " idle=" << stats.idle
        << " in_use=" << stats.inUse
        << " max=" << options.maxSize
        << " waiters=" << stats.waiters
        << " acquire=" << stats.acquireTotal
        << " timeout=" << stats.acquireTimeoutTotal
        << " create=" << stats.createTotal
        << " create_fail=" << stats.createFailTotal
        << " reconnect=" << stats.reconnectTotal
        << " cmd_errors=" << stats.commandErrors;
    return out.str();
}

RedisConn::RedisConn(const std::shared_ptr<detail::RedisCore>& core,
                     RedisCtxHandle ctx,
                     std::chrono::steady_clock::time_point createdAt)
    : m_core(core),
      m_ctx(std::move(ctx)),
      m_createdAt(createdAt),
      m_broken(false),
      m_lastError(ErrorCode::Ok) {
}

bool RedisConn::valid() const {
    return m_ctx.get() != nullptr && !m_broken;
}

void RedisConn::markBroken() {
    m_broken = true;
}

bool RedisConn::ping() {
    if (!valid()) {
        setError(ErrorCode::RedisUnavailable, "connection is closed");
        return false;
    }
    redisReply* raw = redisCommand(m_ctx.get(), "PING");
    if (raw == nullptr || m_ctx->err != 0) {
        markBroken();
        setError(ErrorCode::RedisUnavailable, m_ctx->errstr);
        return false;
    }
    RedisReply reply(raw);
    if (reply.type() != REDIS_REPLY_STATUS || reply.str() != "PONG") {
        setError(ErrorCode::RedisUnavailable, "unexpected PING reply");
        return false;
    }
    return true;
}

RedisLookup RedisConn::get(const std::string& key, std::string* value) {
    if (!valid()) {
        setError(ErrorCode::RedisUnavailable, "connection is closed");
        return RedisLookup::Error;
    }
    redisReply* raw = redisCommand(m_ctx.get(), "GET %s", key.c_str());
    if (raw == nullptr || m_ctx->err != 0) {
        markBroken();
        setError(ErrorCode::RedisUnavailable, m_ctx->errstr);
        return RedisLookup::Error;
    }
    RedisReply reply(raw);
    if (reply.isError()) {
        setError(ErrorCode::RedisUnavailable, reply.str());
        return RedisLookup::Error;
    }
    if (reply.isNil()) {
        return RedisLookup::Miss;
    }
    if (reply.type() == REDIS_REPLY_STRING ||
        reply.type() == REDIS_REPLY_STATUS) {
        if (value != nullptr) {
            *value = reply.str();
        }
        return RedisLookup::Hit;
    }
    setError(ErrorCode::RedisUnavailable, "unexpected GET reply");
    return RedisLookup::Error;
}

bool RedisConn::set(const std::string& key, const std::string& value) {
    if (!valid()) {
        setError(ErrorCode::RedisUnavailable, "connection is closed");
        return false;
    }
    redisReply* raw = redisCommand(m_ctx.get(), "SET %s %b",
                                   key.c_str(), value.data(), value.size());
    if (raw == nullptr || m_ctx->err != 0) {
        markBroken();
        setError(ErrorCode::RedisUnavailable, m_ctx->errstr);
        return false;
    }
    RedisReply reply(raw);
    if (reply.type() == REDIS_REPLY_STATUS && reply.str() == "OK") {
        return true;
    }
    setError(ErrorCode::RedisUnavailable,
             reply.isError() ? reply.str() : std::string("SET failed"));
    return false;
}

bool RedisConn::setPx(const std::string& key, const std::string& value,
                      std::chrono::milliseconds ttl) {
    if (!valid()) {
        setError(ErrorCode::RedisUnavailable, "connection is closed");
        return false;
    }
    redisReply* raw = redisCommand(
        m_ctx.get(), "SET %s %b PX %d",
        key.c_str(), value.data(), value.size(),
        static_cast<int>(ttl.count()));
    if (raw == nullptr || m_ctx->err != 0) {
        markBroken();
        setError(ErrorCode::RedisUnavailable, m_ctx->errstr);
        return false;
    }
    RedisReply reply(raw);
    if (reply.type() == REDIS_REPLY_STATUS && reply.str() == "OK") {
        return true;
    }
    setError(ErrorCode::RedisUnavailable,
             reply.isError() ? reply.str() : std::string("SET PX failed"));
    return false;
}

bool RedisConn::setNxPx(const std::string& key, const std::string& value,
                        std::chrono::milliseconds ttl) {
    if (!valid()) {
        setError(ErrorCode::RedisUnavailable, "connection is closed");
        return false;
    }
    redisReply* raw = redisCommand(
        m_ctx.get(), "SET %s %b NX PX %d",
        key.c_str(), value.data(), value.size(),
        static_cast<int>(ttl.count()));
    if (raw == nullptr || m_ctx->err != 0) {
        markBroken();
        setError(ErrorCode::RedisUnavailable, m_ctx->errstr);
        return false;
    }
    RedisReply reply(raw);
    if (reply.isNil()) {
        return false;
    }
    if (reply.type() == REDIS_REPLY_STATUS && reply.str() == "OK") {
        return true;
    }
    setError(ErrorCode::RedisUnavailable,
             reply.isError() ? reply.str() : std::string("SET NX PX failed"));
    return false;
}

bool RedisConn::expire(const std::string& key, std::chrono::seconds ttl) {
    if (!valid()) {
        setError(ErrorCode::RedisUnavailable, "connection is closed");
        return false;
    }
    redisReply* raw = redisCommand(m_ctx.get(), "EXPIRE %s %d",
                                   key.c_str(),
                                   static_cast<int>(ttl.count()));
    if (raw == nullptr || m_ctx->err != 0) {
        markBroken();
        setError(ErrorCode::RedisUnavailable, m_ctx->errstr);
        return false;
    }
    RedisReply reply(raw);
    if (reply.type() == REDIS_REPLY_INTEGER) {
        if (reply.integer() == 1) {
            return true;
        }
        return false;
    }
    setError(ErrorCode::RedisUnavailable,
             reply.isError() ? reply.str() : std::string("EXPIRE failed"));
    return false;
}

bool RedisConn::hset(const std::string& key, const std::string& field,
                     const std::string& value) {
    if (!valid()) {
        setError(ErrorCode::RedisUnavailable, "connection is closed");
        return false;
    }
    redisReply* raw = redisCommand(m_ctx.get(), "HSET %s %s %b",
                                   key.c_str(), field.c_str(),
                                   value.data(), value.size());
    if (raw == nullptr || m_ctx->err != 0) {
        markBroken();
        setError(ErrorCode::RedisUnavailable, m_ctx->errstr);
        return false;
    }
    RedisReply reply(raw);
    if (reply.type() == REDIS_REPLY_INTEGER) {
        return true;
    }
    setError(ErrorCode::RedisUnavailable,
             reply.isError() ? reply.str() : std::string("HSET failed"));
    return false;
}

RedisLookup RedisConn::hget(const std::string& key, const std::string& field,
                            std::string* value) {
    if (!valid()) {
        setError(ErrorCode::RedisUnavailable, "connection is closed");
        return RedisLookup::Error;
    }
    redisReply* raw = redisCommand(m_ctx.get(), "HGET %s %s",
                                   key.c_str(), field.c_str());
    if (raw == nullptr || m_ctx->err != 0) {
        markBroken();
        setError(ErrorCode::RedisUnavailable, m_ctx->errstr);
        return RedisLookup::Error;
    }
    RedisReply reply(raw);
    if (reply.isError()) {
        setError(ErrorCode::RedisUnavailable, reply.str());
        return RedisLookup::Error;
    }
    if (reply.isNil()) {
        return RedisLookup::Miss;
    }
    if (reply.type() == REDIS_REPLY_STRING ||
        reply.type() == REDIS_REPLY_STATUS) {
        if (value != nullptr) {
            *value = reply.str();
        }
        return RedisLookup::Hit;
    }
    setError(ErrorCode::RedisUnavailable, "unexpected HGET reply");
    return RedisLookup::Error;
}

bool RedisConn::incr(const std::string& key, long long* value) {
    if (!valid()) {
        setError(ErrorCode::RedisUnavailable, "connection is closed");
        return false;
    }
    redisReply* raw = redisCommand(m_ctx.get(), "INCR %s", key.c_str());
    if (raw == nullptr || m_ctx->err != 0) {
        markBroken();
        setError(ErrorCode::RedisUnavailable, m_ctx->errstr);
        return false;
    }
    RedisReply reply(raw);
    if (reply.type() == REDIS_REPLY_INTEGER) {
        if (value != nullptr) {
            *value = reply.integer();
        }
        return true;
    }
    setError(ErrorCode::RedisUnavailable,
             reply.isError() ? reply.str() : std::string("INCR failed"));
    return false;
}

bool RedisConn::del(const std::string& key) {
    if (!valid()) {
        setError(ErrorCode::RedisUnavailable, "connection is closed");
        return false;
    }
    redisReply* raw = redisCommand(m_ctx.get(), "DEL %s", key.c_str());
    if (raw == nullptr || m_ctx->err != 0) {
        markBroken();
        setError(ErrorCode::RedisUnavailable, m_ctx->errstr);
        return false;
    }
    RedisReply reply(raw);
    if (reply.type() == REDIS_REPLY_INTEGER) {
        return true;
    }
    setError(ErrorCode::RedisUnavailable,
             reply.isError() ? reply.str() : std::string("DEL failed"));
    return false;
}

bool RedisConn::eval(const std::string& script, const std::string& key,
                     const std::string& arg, RedisReply* reply) {
    if (!valid()) {
        setError(ErrorCode::RedisUnavailable, "connection is closed");
        return false;
    }
    redisReply* raw = redisCommand(m_ctx.get(), "EVAL %s 1 %s %b",
                                   script.c_str(), key.c_str(),
                                   arg.data(), arg.size());
    if (raw == nullptr || m_ctx->err != 0) {
        markBroken();
        setError(ErrorCode::RedisUnavailable, m_ctx->errstr);
        return false;
    }
    if (raw->type == REDIS_REPLY_ERROR) {
        const std::string message = std::string(raw->str, raw->len);
        freeReplyObject(raw);
        setError(ErrorCode::RedisUnavailable, message);
        return false;
    }
    if (reply != nullptr) {
        reply->reset(raw);
    } else {
        freeReplyObject(raw);
    }
    return true;
}

namespace detail {

RedisCore::RedisCore(const RedisOptions& opts)
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
      reconnectTotal(0),
      commandErrors(0) {
}

RedisCore::~RedisCore() {
    shutdown();
}

void RedisCore::startReaper() {
    reaper = std::thread(&RedisCore::reaperLoop, this);
}

void RedisCore::giveBack(RedisCtxHandle ctx,
                         std::chrono::steady_clock::time_point createdAt,
                         std::chrono::steady_clock::time_point lastActive,
                         bool broken) {
    RedisCtxHandle closing;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (inUse > 0) {
            --inUse;
        }
        const bool expired =
            (std::chrono::steady_clock::now() - createdAt) > options.maxLifetime;
        if (broken || stopping || expired || !ctx) {
            if (total > 0) {
                --total;
            }
            if (broken) {
                reconnectTotal.fetch_add(1, std::memory_order_relaxed);
            }
            closing = std::move(ctx);
        } else {
            IdleRedis slot;
            slot.ctx = std::move(ctx);
            slot.createdAt = createdAt;
            slot.lastActive = lastActive;
            idle.push_back(std::move(slot));
        }
        cv.notify_one();
    }
}

void RedisCore::shutdown() {
    std::deque<IdleRedis> closing;
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

void RedisCore::reaperLoop() {
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
        std::deque<IdleRedis> expired;
        std::deque<IdleRedis>::iterator it = idle.begin();
        while (it != idle.end()) {
            const bool tooOld = (now - it->createdAt) > options.maxLifetime;
            const bool tooIdle = (now - it->lastActive) > options.idleTimeout;
            if (tooOld || (tooIdle && total > options.minSize)) {
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
    }
}

}  // namespace detail

RedisConn::~RedisConn() {
    if (!m_core) {
        return;
    }
    m_core->giveBack(std::move(m_ctx), m_createdAt,
                     std::chrono::steady_clock::now(), m_broken);
}

void RedisConn::setError(ErrorCode code, const std::string& message) {
    m_lastError = code;
    m_lastErrorMsg = message;
    if (m_core) {
        m_core->commandErrors.fetch_add(1, std::memory_order_relaxed);
    }
}

bool RedisConn::reconnect() {
    m_ctx.reset();
    std::string error;
    RedisCtxHandle ctx = createPhysical(m_core->options, &error);
    if (!ctx) {
        markBroken();
        setError(ErrorCode::RedisUnavailable, error);
        return false;
    }
    m_ctx = std::move(ctx);
    m_createdAt = std::chrono::steady_clock::now();
    m_broken = false;
    m_lastError = ErrorCode::Ok;
    m_lastErrorMsg.clear();
    return true;
}

RedisPool::RedisPool(const RedisOptions& options,
                     const std::shared_ptr<detail::RedisCore>& core)
    : m_core(core),
      m_options(options),
      m_lastError(static_cast<int>(ErrorCode::Ok)) {
}

RedisPool::~RedisPool() {
    if (m_core) {
        m_core->shutdown();
    }
}

std::shared_ptr<RedisPool> RedisPool::create(const RedisOptions& options,
                                             std::string* err) {
    std::string localError;
    if (!options.validate(&localError)) {
        if (err != nullptr) {
            *err = localError;
        }
        return std::shared_ptr<RedisPool>();
    }

    std::shared_ptr<detail::RedisCore> core =
        std::make_shared<detail::RedisCore>(options);

    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < options.minSize; ++i) {
        RedisCtxHandle ctx = createPhysical(options, &localError);
        if (!ctx) {
            if (err != nullptr) {
                *err = localError;
            }
            return std::shared_ptr<RedisPool>();
        }
        detail::IdleRedis slot;
        slot.ctx = std::move(ctx);
        slot.createdAt = now;
        slot.lastActive = now;
        core->idle.push_back(std::move(slot));
        ++core->total;
    }

    core->startReaper();
    if (err != nullptr) {
        err->clear();
    }
    return std::shared_ptr<RedisPool>(new RedisPool(options, core));
}

std::unique_ptr<RedisConn> RedisPool::acquire() {
    return acquire(m_options.acquireTimeout);
}

std::unique_ptr<RedisConn> RedisPool::acquire(std::chrono::milliseconds timeout) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(m_core->mutex);

    for (;;) {
        if (m_core->stopping) {
            setLastError(ErrorCode::RedisUnavailable);
            return std::unique_ptr<RedisConn>();
        }
        if (std::chrono::steady_clock::now() < m_core->circuitOpenUntil) {
            setLastError(ErrorCode::RedisUnavailable);
            return std::unique_ptr<RedisConn>();
        }

        if (!m_core->idle.empty()) {
            detail::IdleRedis slot = std::move(m_core->idle.front());
            m_core->idle.pop_front();
            ++m_core->inUse;
            lock.unlock();

            const std::chrono::steady_clock::time_point now =
                std::chrono::steady_clock::now();
            bool healthy = slot.ctx.get() != nullptr && slot.ctx->err == 0 &&
                           (now - slot.createdAt) <= m_core->options.maxLifetime;
            if (healthy && now - slot.lastActive > std::chrono::seconds(30)) {
                redisReply* raw = redisCommand(slot.ctx.get(), "PING");
                healthy = (raw != nullptr && slot.ctx->err == 0 &&
                           raw->type == REDIS_REPLY_STATUS);
                if (raw != nullptr) {
                    freeReplyObject(raw);
                }
            }

            if (!healthy) {
                slot.ctx.reset();
                lock.lock();
                if (m_core->total > 0) {
                    --m_core->total;
                }
                if (m_core->inUse > 0) {
                    --m_core->inUse;
                }
                m_core->reconnectTotal.fetch_add(1, std::memory_order_relaxed);
                m_core->cv.notify_one();
                if (std::chrono::steady_clock::now() >= deadline) {
                    setLastError(ErrorCode::RedisUnavailable);
                    return std::unique_ptr<RedisConn>();
                }
                continue;
            }

            m_core->acquireTotal.fetch_add(1, std::memory_order_relaxed);
            return std::unique_ptr<RedisConn>(
                new RedisConn(m_core, std::move(slot.ctx), slot.createdAt));
        }

        if (m_core->total < m_core->options.maxSize) {
            ++m_core->total;
            ++m_core->inUse;
            lock.unlock();

            std::string connectError;
            RedisCtxHandle ctx = createPhysical(m_core->options, &connectError);
            if (!ctx) {
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
                m_core->cv.notify_one();
                setLastError(ErrorCode::RedisUnavailable);
                return std::unique_ptr<RedisConn>();
            }

            m_core->consecutiveCreateFails = 0;
            m_core->createTotal.fetch_add(1, std::memory_order_relaxed);
            m_core->acquireTotal.fetch_add(1, std::memory_order_relaxed);
            return std::unique_ptr<RedisConn>(
                new RedisConn(m_core, std::move(ctx),
                              std::chrono::steady_clock::now()));
        }

        if (timeout.count() <= 0 ||
            std::chrono::steady_clock::now() >= deadline) {
            m_core->acquireTimeoutTotal.fetch_add(1, std::memory_order_relaxed);
            setLastError(ErrorCode::RedisUnavailable);
            return std::unique_ptr<RedisConn>();
        }

        ++m_core->waiters;
        const bool signaled = m_core->cv.wait_until(lock, deadline, [this]() {
            return m_core->stopping || !m_core->idle.empty();
        });
        --m_core->waiters;

        if (!signaled) {
            m_core->acquireTimeoutTotal.fetch_add(1, std::memory_order_relaxed);
            setLastError(ErrorCode::RedisUnavailable);
            return std::unique_ptr<RedisConn>();
        }
    }
}

RedisPoolStats RedisPool::stats() const {
    RedisPoolStats result;
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
    result.reconnectTotal =
        m_core->reconnectTotal.load(std::memory_order_relaxed);
    result.commandErrors =
        m_core->commandErrors.load(std::memory_order_relaxed);
    return result;
}

std::string RedisPool::describe() const {
    return formatRedisDescription(m_options, stats());
}

void RedisPool::shutdown() {
    if (m_core) {
        m_core->shutdown();
    }
}

ErrorCode RedisPool::lastError() const {
    return static_cast<ErrorCode>(m_lastError.load(std::memory_order_relaxed));
}

void RedisPool::setLastError(ErrorCode code) {
    m_lastError.store(static_cast<int>(code), std::memory_order_relaxed);
}

}  // namespace cache
}  // namespace cm
