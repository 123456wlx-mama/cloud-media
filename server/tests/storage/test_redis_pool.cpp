#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cache/cache_store.h"
#include "cache/redis_pool.h"

namespace {

bool envFlag(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && std::string(value) == "1";
}

std::string envOr(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string(fallback);
}

cm::cache::RedisOptions redisOptions() {
    cm::cache::RedisOptions options;
    options.host = envOr("CM_TEST_REDIS_HOST", "127.0.0.1");
    options.port = std::atoi(envOr("CM_TEST_REDIS_PORT", "6379").c_str());
    options.password = envOr("CM_TEST_REDIS_PASSWORD", "");
    options.db = std::atoi(envOr("CM_TEST_REDIS_DB", "0").c_str());
    options.minSize = 1;
    options.maxSize = 4;
    return options;
}

std::string uniqueKey(const char* name) {
    static std::atomic<std::uint64_t> sequence(0);
    const std::uint64_t now = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::uint64_t seq = sequence.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream out;
    out << "cm:test:" << std::hex << now << ":" << name << ":" << seq;
    return out.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// 离线单测：不连接 Redis
// ---------------------------------------------------------------------------

TEST(CacheTtlTest, Boundaries) {
    EXPECT_EQ(std::int64_t(900), cm::cache::jitterTtlMs(1000, 10, -10));
    EXPECT_EQ(std::int64_t(1000), cm::cache::jitterTtlMs(1000, 10, 0));
    EXPECT_EQ(std::int64_t(1100), cm::cache::jitterTtlMs(1000, 10, 10));
}

TEST(CacheTtlTest, ClampsOutOfRangePercent) {
    EXPECT_EQ(std::int64_t(900), cm::cache::jitterTtlMs(1000, 10, -999));
    EXPECT_EQ(std::int64_t(1100), cm::cache::jitterTtlMs(1000, 10, 999));
    EXPECT_EQ(std::int64_t(1000), cm::cache::jitterTtlMs(1000, 0, 10));
}

TEST(CacheTtlTest, RandomDistributionStaysInRange) {
    std::set<std::int64_t> distinct;
    for (int i = 0; i < 100000; ++i) {
        const std::int64_t ttl =
            cm::cache::randomJitterTtlMs(1000, 10);
        EXPECT_GE(ttl, std::int64_t(900));
        EXPECT_LE(ttl, std::int64_t(1100));
        distinct.insert(ttl);
    }
    EXPECT_GE(distinct.size(), std::size_t(15));
}

TEST(CacheTtlTest, MinimumIsOneMillisecond) {
    EXPECT_EQ(std::int64_t(1), cm::cache::jitterTtlMs(1, 10, -10));
    EXPECT_EQ(std::int64_t(0), cm::cache::jitterTtlMs(0, 10, 10));
}

TEST(CacheKeyTest, MatchesSpecFormats) {
    const std::string md5(32, 'a');
    EXPECT_EQ(std::string("cm:v1:sess:token"),
              cm::cache::CacheKey::session("token"));
    EXPECT_EQ(std::string("cm:v1:usr:42"), cm::cache::CacheKey::user(42));
    EXPECT_EQ(std::string("cm:v1:file:md5:") + md5,
              cm::cache::CacheKey::fileByMd5(md5));
    EXPECT_EQ(std::string("cm:v1:file:id:7"),
              cm::cache::CacheKey::fileById(7));
    // Phase 5 起分享码固定 6 位（31 字符表，去掉 0/O/1/I/L），9 位旧码直接判非法返回空
    EXPECT_EQ(std::string("cm:v1:share:AB2CD3"),
              cm::cache::CacheKey::share("AB2CD3"));
    EXPECT_EQ(std::string("cm:v1:share:miss:AB2CD3"),
              cm::cache::CacheKey::shareMiss("AB2CD3"));
    EXPECT_EQ(std::string(""), cm::cache::CacheKey::share("123456789"));
    EXPECT_EQ(std::string("cm:v1:up:bitmap:task"),
              cm::cache::CacheKey::uploadBitmap("task"));
    EXPECT_EQ(std::string("cm:v1:rl:ip:127.0.0.1:60"),
              cm::cache::CacheKey::rateLimitIp("127.0.0.1", 60));
    EXPECT_EQ(std::string("cm:v1:lock:rebuild:cm:v1:usr:42"),
              cm::cache::CacheKey::rebuildLock("cm:v1:usr:42"));
    EXPECT_EQ(std::string("cm:v1:dl:ticket:tk"),
              cm::cache::CacheKey::downloadTicket("tk"));
}

TEST(CacheKeyTest, RejectsUnsafeValues) {
    EXPECT_TRUE(cm::cache::CacheKey::session("").empty());
    EXPECT_TRUE(cm::cache::CacheKey::session("a b").empty());
    EXPECT_TRUE(cm::cache::CacheKey::session("a\nb").empty());
    EXPECT_TRUE(cm::cache::CacheKey::session("a\rb").empty());
    EXPECT_TRUE(cm::cache::CacheKey::share("123").empty());
    EXPECT_TRUE(cm::cache::CacheKey::share("12345678a").empty());
    EXPECT_TRUE(cm::cache::CacheKey::fileByMd5("xyz").empty());
    EXPECT_TRUE(cm::cache::CacheKey::rebuildLock("").empty());
}

TEST(NullMarkerTest, Protocol) {
    EXPECT_STREQ("__cm_null__", cm::cache::CacheStore::nullMarker());
    EXPECT_TRUE(cm::cache::CacheStore::isNullMarker("__cm_null__"));
    EXPECT_FALSE(cm::cache::CacheStore::isNullMarker(""));
    EXPECT_FALSE(cm::cache::CacheStore::isNullMarker("__CM_nullptr__"));
}

TEST(CacheStatsTest, HitRate) {
    cm::cache::CacheStats stats;
    stats.hits = 8;
    stats.misses = 2;
    EXPECT_DOUBLE_EQ(0.8, stats.hitRate());

    cm::cache::CacheStats empty;
    EXPECT_DOUBLE_EQ(0.0, empty.hitRate());
}

TEST(RedisOptionsTest, DefaultsMatchSpec) {
    cm::cache::RedisOptions options;
    EXPECT_EQ(std::string("127.0.0.1"), options.host);
    EXPECT_EQ(6379, options.port);
    EXPECT_EQ(0, options.db);
    EXPECT_EQ(200, static_cast<int>(options.connectTimeout.count()));
    EXPECT_EQ(300, static_cast<int>(options.commandTimeout.count()));
    EXPECT_EQ(std::size_t(2), options.minSize);
    EXPECT_EQ(std::size_t(8), options.maxSize);
    EXPECT_EQ(60000, static_cast<int>(options.idleTimeout.count()));
    EXPECT_EQ(200, static_cast<int>(options.acquireTimeout.count()));

    std::string error;
    EXPECT_TRUE(options.validate(&error)) << error;
}

TEST(RedisDescribeTest, RedactsPassword) {
    cm::cache::RedisOptions options;
    options.password = "s3cr3t-value";
    cm::cache::RedisPoolStats stats;
    stats.total = 3;
    stats.idle = 2;
    stats.inUse = 1;
    stats.acquireTotal = 9;

    const std::string text = cm::cache::formatRedisDescription(options, stats);
    EXPECT_EQ(std::string::npos, text.find("s3cr3t-value"));
    EXPECT_NE(std::string::npos, text.find("host=127.0.0.1"));
    EXPECT_NE(std::string::npos, text.find("pool total=3"));
    EXPECT_NE(std::string::npos, text.find("acquire=9"));
}

// ---------------------------------------------------------------------------
// 集成测试：未设置 CM_TEST_REDIS 时跳过；设置后连不上必须 FAIL
// ---------------------------------------------------------------------------

class RedisIntegration : public ::testing::Test {
protected:
    void SetUp() override {
        if (!envFlag("CM_TEST_REDIS")) {
            GTEST_SKIP() << "set CM_TEST_REDIS=1 to run Redis integration tests";
        }
        std::string error;
        m_pool = cm::cache::RedisPool::create(redisOptions(), &error);
        if (!m_pool) {
            FAIL() << "Redis connection failed: " << error;
        }
    }

    std::shared_ptr<cm::cache::RedisPool> m_pool;
};

TEST_F(RedisIntegration, ConnectAndPing) {
    std::unique_ptr<cm::cache::RedisConn> conn = m_pool->acquire();
    ASSERT_TRUE(conn);
    EXPECT_TRUE(conn->valid());
    EXPECT_TRUE(conn->ping());
}

TEST_F(RedisIntegration, SetGetDel) {
    std::unique_ptr<cm::cache::RedisConn> conn = m_pool->acquire();
    ASSERT_TRUE(conn);

    const std::string key = uniqueKey("setget");
    ASSERT_TRUE(conn->set(key, "value"));
    std::string value;
    EXPECT_TRUE(conn->get(key, &value) == cm::cache::RedisLookup::Hit);
    EXPECT_EQ(std::string("value"), value);
    ASSERT_TRUE(conn->del(key));
    EXPECT_TRUE(conn->get(key, &value) == cm::cache::RedisLookup::Miss);
}

TEST_F(RedisIntegration, SetPxExpires) {
    std::unique_ptr<cm::cache::RedisConn> conn = m_pool->acquire();
    ASSERT_TRUE(conn);

    const std::string key = uniqueKey("setpx");
    ASSERT_TRUE(conn->setPx(key, "value", std::chrono::milliseconds(100)));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    std::string value;
    EXPECT_TRUE(conn->get(key, &value) == cm::cache::RedisLookup::Miss);
    conn->del(key);
}

TEST_F(RedisIntegration, SetNxPxOnlyOnce) {
    std::unique_ptr<cm::cache::RedisConn> conn = m_pool->acquire();
    ASSERT_TRUE(conn);

    const std::string key = uniqueKey("setnx");
    EXPECT_TRUE(conn->setNxPx(key, "one", std::chrono::milliseconds(1000)));
    EXPECT_FALSE(conn->setNxPx(key, "two", std::chrono::milliseconds(1000)));
    conn->del(key);
}

TEST_F(RedisIntegration, IncrAndHset) {
    std::unique_ptr<cm::cache::RedisConn> conn = m_pool->acquire();
    ASSERT_TRUE(conn);

    const std::string counter = uniqueKey("incr");
    long long value = 0;
    ASSERT_TRUE(conn->incr(counter, &value));
    EXPECT_EQ(1, value);
    ASSERT_TRUE(conn->incr(counter, &value));
    EXPECT_EQ(2, value);
    conn->del(counter);

    const std::string hash = uniqueKey("hash");
    ASSERT_TRUE(conn->hset(hash, "field", "value"));
    std::string fieldValue;
    EXPECT_TRUE(conn->hget(hash, "field", &fieldValue) ==
                cm::cache::RedisLookup::Hit);
    EXPECT_EQ(std::string("value"), fieldValue);
    EXPECT_TRUE(conn->hget(hash, "missing", &fieldValue) ==
                cm::cache::RedisLookup::Miss);
    conn->del(hash);
}

TEST_F(RedisIntegration, EvalCompareDel) {
    std::unique_ptr<cm::cache::RedisConn> conn = m_pool->acquire();
    ASSERT_TRUE(conn);

    const std::string key = uniqueKey("lock");
    ASSERT_TRUE(conn->set(key, "owner"));

    const std::string script =
        "if redis.call('GET', KEYS[1]) == ARGV[1] then "
        "return redis.call('DEL', KEYS[1]) else return 0 end";

    cm::cache::RedisReply reply;
    ASSERT_TRUE(conn->eval(script, key, "wrong", &reply));
    EXPECT_EQ(0, reply.integer());

    ASSERT_TRUE(conn->eval(script, key, "owner", &reply));
    EXPECT_EQ(1, reply.integer());

    std::string value;
    EXPECT_TRUE(conn->get(key, &value) == cm::cache::RedisLookup::Miss);
}

TEST_F(RedisIntegration, CacheStoreNullMarkerExpires) {
    cm::cache::CacheStoreOptions options;
    options.defaultTtl = std::chrono::milliseconds(1000);
    options.nullTtl = std::chrono::milliseconds(200);
    cm::cache::CacheStore store(*m_pool, options);

    const std::string key = uniqueKey("null");
    int loads = 0;
    cm::cache::CacheStore::Loader loader =
        [&loads](std::string*) -> cm::cache::CacheStore::LoadStatus {
            ++loads;
            return cm::cache::CacheStore::LoadStatus::NotFound;
        };

    std::string out;
    EXPECT_TRUE(store.getOrLoad(key, loader, &out) ==
                cm::cache::CacheStore::Result::Miss);
    EXPECT_EQ(1, loads);

    EXPECT_TRUE(store.getOrLoad(key, loader, &out) ==
                cm::cache::CacheStore::Result::Miss);
    EXPECT_EQ(1, loads);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_TRUE(store.getOrLoad(key, loader, &out) ==
                cm::cache::CacheStore::Result::Miss);
    EXPECT_EQ(2, loads);

    store.invalidate(key);
}

TEST_F(RedisIntegration, CacheStoreHitSkipsLoader) {
    cm::cache::CacheStoreOptions options;
    cm::cache::CacheStore store(*m_pool, options);

    const std::string key = uniqueKey("hit");
    ASSERT_TRUE(store.put(key, "cached", std::chrono::milliseconds(1000)));

    int loads = 0;
    cm::cache::CacheStore::Loader loader =
        [&loads](std::string* out) -> cm::cache::CacheStore::LoadStatus {
            ++loads;
            if (out != nullptr) {
                *out = "loaded";
            }
            return cm::cache::CacheStore::LoadStatus::Found;
        };

    std::string out;
    EXPECT_TRUE(store.getOrLoad(key, loader, &out) ==
                cm::cache::CacheStore::Result::Hit);
    EXPECT_EQ(std::string("cached"), out);
    EXPECT_EQ(0, loads);

    store.invalidate(key);
}

TEST_F(RedisIntegration, CacheStoreMissLoadsOnce) {
    cm::cache::CacheStoreOptions options;
    cm::cache::CacheStore store(*m_pool, options);

    const std::string key = uniqueKey("miss");
    int loads = 0;
    cm::cache::CacheStore::Loader loader =
        [&loads](std::string* out) -> cm::cache::CacheStore::LoadStatus {
            ++loads;
            if (out != nullptr) {
                *out = "loaded";
            }
            return cm::cache::CacheStore::LoadStatus::Found;
        };

    std::string out;
    EXPECT_TRUE(store.getOrLoad(key, loader, &out) ==
                cm::cache::CacheStore::Result::Hit);
    EXPECT_EQ(std::string("loaded"), out);
    EXPECT_EQ(1, loads);

    EXPECT_TRUE(store.getOrLoad(key, loader, &out) ==
                cm::cache::CacheStore::Result::Hit);
    EXPECT_EQ(1, loads);

    store.invalidate(key);
}

// 并发单飞：8 个线程同时请求同一个未缓存的 key，loader 只应执行一次；
// 且等待者必须在 200ms 上限内拿到 leader 的结果（不能串行化整个 loader）。
TEST_F(RedisIntegration, ConcurrentSameKeyLoadsOnce) {
    cm::cache::CacheStoreOptions options;
    cm::cache::CacheStore store(*m_pool, options);

    const std::string key = uniqueKey("singleflight");
    std::atomic<int> loads(0);
    cm::cache::CacheStore::Loader loader =
        [&loads](std::string* out) -> cm::cache::CacheStore::LoadStatus {
            loads.fetch_add(1);
            // 模拟慢 loader：修复前分片锁会把它串行化，现在其他线程应等待并复用结果
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (out != nullptr) {
                *out = "singleflight-value";
            }
            return cm::cache::CacheStore::LoadStatus::Found;
        };

    const int threadCount = 8;
    std::atomic<int> okCount(0);
    std::vector<std::thread> threads;
    for (int i = 0; i < threadCount; ++i) {
        threads.push_back(std::thread([&store, &loader, &key, &okCount]() {
            std::string value;
            if (store.getOrLoad(key, loader, &value) ==
                    cm::cache::CacheStore::Result::Hit &&
                value == "singleflight-value") {
                okCount.fetch_add(1);
            }
        }));
    }
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }

    EXPECT_EQ(threadCount, okCount.load());
    EXPECT_EQ(1, loads.load());

    store.invalidate(key);
}
