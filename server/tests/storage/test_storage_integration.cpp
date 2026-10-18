#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cache/cache_store.h"
#include "cache/redis_pool.h"
#include "db/mysql_pool.h"
#include "db/prepared_statement.h"

namespace {

bool envFlag(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && std::string(value) == "1";
}

std::string envOr(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string(fallback);
}

cm::db::MysqlOptions mysqlOptions() {
    cm::db::MysqlOptions options;
    options.host = envOr("CM_TEST_MYSQL_HOST", "127.0.0.1");
    options.port = static_cast<unsigned short>(
        std::atoi(envOr("CM_TEST_MYSQL_PORT", "3306").c_str()));
    options.user = envOr("CM_TEST_MYSQL_USER", "cm_test");
    options.password = envOr("CM_TEST_MYSQL_PASSWORD", "");
    options.database = envOr("CM_TEST_MYSQL_DB", "cloud_media_test");
    options.minSize = 4;
    options.maxSize = 8;
    return options;
}

cm::cache::RedisOptions redisOptions() {
    cm::cache::RedisOptions options;
    options.host = envOr("CM_TEST_REDIS_HOST", "127.0.0.1");
    options.port = std::atoi(envOr("CM_TEST_REDIS_PORT", "6379").c_str());
    options.password = envOr("CM_TEST_REDIS_PASSWORD", "");
    options.db = std::atoi(envOr("CM_TEST_REDIS_DB", "0").c_str());
    options.minSize = 4;
    options.maxSize = 8;
    return options;
}

std::string uniqueTel() {
    static std::atomic<std::uint64_t> sequence(0);
    const std::uint64_t now = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::uint64_t value =
        1000000000ULL + ((now + sequence.fetch_add(1, std::memory_order_relaxed))
                         % 9000000000ULL);
    return std::string("1") + std::to_string(value);
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

class MysqlStorageIntegration : public ::testing::Test {
protected:
    void SetUp() override {
        if (!envFlag("CM_TEST_MYSQL")) {
            GTEST_SKIP() << "set CM_TEST_MYSQL=1 to run MySQL storage integration tests";
        }
        std::string error;
        m_pool = cm::db::MysqlPool::create(mysqlOptions(), &error);
        if (!m_pool) {
            FAIL() << "MySQL connection failed: " << error;
        }
    }

    std::shared_ptr<cm::db::MysqlPool> m_pool;
};

class RedisStorageIntegration : public ::testing::Test {
protected:
    void SetUp() override {
        if (!envFlag("CM_TEST_REDIS")) {
            GTEST_SKIP() << "set CM_TEST_REDIS=1 to run Redis storage integration tests";
        }
        std::string error;
        m_pool = cm::cache::RedisPool::create(redisOptions(), &error);
        if (!m_pool) {
            FAIL() << "Redis connection failed: " << error;
        }
    }

    std::shared_ptr<cm::cache::RedisPool> m_pool;
};

TEST_F(MysqlStorageIntegration, PoolConcurrentBorrowReturn) {
    const int kThreads = 8;
    const int kIterations = 50;
    std::atomic<int> failures(0);
    std::vector<std::thread> workers;

    for (int i = 0; i < kThreads; ++i) {
        workers.push_back(std::thread([this, &failures]() {
            for (int j = 0; j < 50; ++j) {
                std::unique_ptr<cm::db::MysqlConn> conn = m_pool->acquire();
                if (!conn || !conn->valid()) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }));
    }
    for (std::size_t i = 0; i < workers.size(); ++i) {
        workers[i].join();
    }

    EXPECT_EQ(0, failures.load());
    EXPECT_EQ(std::uint64_t(kThreads * kIterations),
              m_pool->stats().acquireTotal);

    const cm::db::MysqlPoolStats stats = m_pool->stats();
    EXPECT_EQ(std::size_t(0), stats.inUse);
    EXPECT_LE(stats.total, std::size_t(8));
}

TEST_F(MysqlStorageIntegration, TransactionCommitAndRollback) {
    std::unique_ptr<cm::db::MysqlConn> conn = m_pool->acquire();
    ASSERT_TRUE(conn) << static_cast<int>(m_pool->lastError());

    const std::string telCommit = uniqueTel();
    const std::string telRollback = uniqueTel();
    const std::string password(64, 'a');
    const std::string salt(32, 'b');

    {
        cm::db::Transaction tx(*conn);
        ASSERT_TRUE(tx.valid()) << conn->lastErrorMessage();

        std::unique_ptr<cm::db::PreparedStatement> insert = conn->prepare(
            "INSERT INTO t_user (u_tel, u_password, u_salt, u_name) "
            "VALUES (?, ?, ?, ?)");
        ASSERT_TRUE(insert) << conn->lastErrorMessage();
        ASSERT_TRUE(insert->bindString(0, telCommit));
        ASSERT_TRUE(insert->bindString(1, password));
        ASSERT_TRUE(insert->bindString(2, salt));
        ASSERT_TRUE(insert->bindString(3, "integration"));
        ASSERT_TRUE(insert->execute()) << insert->lastErrorMessage();

        ASSERT_TRUE(tx.commit()) << conn->lastErrorMessage();
    }

    {
        std::unique_ptr<cm::db::PreparedStatement> count =
            conn->prepare("SELECT COUNT(*) FROM t_user WHERE u_tel = ?");
        ASSERT_TRUE(count) << conn->lastErrorMessage();
        ASSERT_TRUE(count->bindString(0, telCommit));
        ASSERT_TRUE(count->execute()) << count->lastErrorMessage();
        ASSERT_TRUE(count->fetch());
        EXPECT_EQ(std::int64_t(1), count->getInt64(0));
    }

    {
        cm::db::Transaction tx(*conn);
        ASSERT_TRUE(tx.valid()) << conn->lastErrorMessage();

        std::unique_ptr<cm::db::PreparedStatement> insert = conn->prepare(
            "INSERT INTO t_user (u_tel, u_password, u_salt, u_name) "
            "VALUES (?, ?, ?, ?)");
        ASSERT_TRUE(insert) << conn->lastErrorMessage();
        ASSERT_TRUE(insert->bindString(0, telRollback));
        ASSERT_TRUE(insert->bindString(1, password));
        ASSERT_TRUE(insert->bindString(2, salt));
        ASSERT_TRUE(insert->bindString(3, "integration"));
        ASSERT_TRUE(insert->execute()) << insert->lastErrorMessage();
        // 离开作用域时 Transaction 析构回滚。
    }

    {
        std::unique_ptr<cm::db::PreparedStatement> count =
            conn->prepare("SELECT COUNT(*) FROM t_user WHERE u_tel = ?");
        ASSERT_TRUE(count) << conn->lastErrorMessage();
        ASSERT_TRUE(count->bindString(0, telRollback));
        ASSERT_TRUE(count->execute()) << count->lastErrorMessage();
        ASSERT_TRUE(count->fetch());
        EXPECT_EQ(std::int64_t(0), count->getInt64(0));
    }

    {
        std::unique_ptr<cm::db::PreparedStatement> remove = conn->prepare(
            "DELETE FROM t_user WHERE u_tel IN (?, ?)");
        ASSERT_TRUE(remove) << conn->lastErrorMessage();
        ASSERT_TRUE(remove->bindString(0, telCommit));
        ASSERT_TRUE(remove->bindString(1, telRollback));
        ASSERT_TRUE(remove->execute()) << remove->lastErrorMessage();
    }
}

TEST_F(RedisStorageIntegration, SetGetExpire) {
    std::unique_ptr<cm::cache::RedisConn> conn = m_pool->acquire();
    ASSERT_TRUE(conn) << static_cast<int>(m_pool->lastError());

    const std::string key = uniqueKey("expire");
    ASSERT_TRUE(conn->set(key, "value"));

    std::string value;
    EXPECT_TRUE(conn->get(key, &value) == cm::cache::RedisLookup::Hit);
    EXPECT_EQ(std::string("value"), value);

    ASSERT_TRUE(conn->expire(key, std::chrono::seconds(1)));
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    EXPECT_TRUE(conn->get(key, &value) == cm::cache::RedisLookup::Miss);

    conn->del(key);
}

TEST_F(RedisStorageIntegration, CacheStoreHitRate) {
    cm::cache::CacheStoreOptions options;
    cm::cache::CacheStore store(*m_pool, options);

    const std::string hitKey = uniqueKey("cache_hit");
    ASSERT_TRUE(store.put(hitKey, "cached", std::chrono::milliseconds(1000)));

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
    EXPECT_TRUE(store.getOrLoad(hitKey, loader, &out) ==
                cm::cache::CacheStore::Result::Hit);
    EXPECT_EQ(std::string("cached"), out);
    EXPECT_EQ(0, loads);

    const std::string missKey = uniqueKey("cache_miss");
    EXPECT_TRUE(store.getOrLoad(missKey, loader, &out) ==
                cm::cache::CacheStore::Result::Hit);
    EXPECT_EQ(std::string("loaded"), out);
    EXPECT_EQ(1, loads);

    const cm::cache::CacheStats stats = store.stats();
    EXPECT_GE(stats.hits, std::uint64_t(1));
    EXPECT_GE(stats.misses, std::uint64_t(1));
    EXPECT_GE(stats.loads, std::uint64_t(1));
    EXPECT_GT(stats.hitRate(), 0.0);
    EXPECT_LE(stats.hitRate(), 1.0);

    store.invalidate(hitKey);
    store.invalidate(missKey);
}
