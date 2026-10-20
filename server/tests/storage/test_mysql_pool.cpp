#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

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

cm::db::MysqlOptions integrationOptions() {
    cm::db::MysqlOptions options;
    options.host = envOr("CM_TEST_MYSQL_HOST", "127.0.0.1");
    options.port = static_cast<unsigned short>(
        std::atoi(envOr("CM_TEST_MYSQL_PORT", "3306").c_str()));
    options.user = envOr("CM_TEST_MYSQL_USER", "cm_test");
    options.password = envOr("CM_TEST_MYSQL_PASSWORD", "");
    options.database = envOr("CM_TEST_MYSQL_DB", "cloud_media_test");
    options.minSize = 1;
    options.maxSize = 4;
    return options;
}

}  // namespace

// ---------------------------------------------------------------------------
// 离线单测：不连接 MySQL
// ---------------------------------------------------------------------------

TEST(MysqlOptionsTest, DefaultsMatchSpec) {
    cm::db::MysqlOptions options;
    EXPECT_EQ(3306, static_cast<int>(options.port));
    EXPECT_EQ(std::size_t(2), options.minSize);
    EXPECT_EQ(std::size_t(8), options.maxSize);
    EXPECT_EQ(60000, static_cast<int>(options.idleTimeout.count()));
    EXPECT_EQ(500, static_cast<int>(options.acquireTimeout.count()));
    EXPECT_EQ(1800000, static_cast<int>(options.maxLifetime.count()));
    EXPECT_EQ(200, static_cast<int>(options.slowQueryThreshold.count()));
    EXPECT_EQ(std::string("utf8mb4"), options.charset);

    options.host = "127.0.0.1";
    options.user = "cm_test";
    options.database = "cloud_media_test";
    std::string error;
    EXPECT_TRUE(options.validate(&error)) << error;
}

TEST(MysqlOptionsTest, RejectsInvalidPoolRange) {
    std::string error;
    cm::db::MysqlOptions options = cm::db::MysqlOptions();
    options.host = "127.0.0.1";
    options.user = "cm_test";
    options.database = "cloud_media_test";

    options.minSize = 0;
    EXPECT_FALSE(options.validate(&error));
    EXPECT_FALSE(error.empty());

    options = cm::db::MysqlOptions();
    options.host = "127.0.0.1";
    options.user = "cm_test";
    options.database = "cloud_media_test";
    options.minSize = 4;
    options.maxSize = 2;
    EXPECT_FALSE(options.validate(&error));

    options = cm::db::MysqlOptions();
    options.host = "127.0.0.1";
    options.user = "cm_test";
    options.database = "cloud_media_test";
    options.maxSize = 33;
    EXPECT_FALSE(options.validate(&error));

    options = cm::db::MysqlOptions();
    options.host = "127.0.0.1";
    options.user = "cm_test";
    options.database = "cloud_media_test";
    options.maxSize = 33;
    std::string createError;
    std::shared_ptr<cm::db::MysqlPool> pool =
        cm::db::MysqlPool::create(options, &createError);
    EXPECT_FALSE(pool);
    EXPECT_FALSE(createError.empty());
}

TEST(MysqlOptionsTest, RejectsInvalidConnectionFields) {
    std::string error;
    cm::db::MysqlOptions options;

    options.host.clear();
    options.user = "u";
    options.database = "d";
    EXPECT_FALSE(options.validate(&error));

    options.host = "127.0.0.1";
    options.user.clear();
    EXPECT_FALSE(options.validate(&error));

    options.user = "u";
    options.database.clear();
    EXPECT_FALSE(options.validate(&error));
}

TEST(MysqlDescribeTest, RedactsPassword) {
    cm::db::MysqlOptions options;
    options.host = "127.0.0.1";
    options.user = "cm_app";
    options.database = "cloud_media";
    options.password = "s3cr3t-value";

    cm::db::MysqlPoolStats stats;
    stats.total = 4;
    stats.idle = 3;
    stats.inUse = 1;
    stats.acquireTotal = 10;
    stats.slowQueryTotal = 2;

    const std::string text = cm::db::formatMysqlDescription(options, stats);
    EXPECT_EQ(std::string::npos, text.find("s3cr3t-value"));
    EXPECT_NE(std::string::npos, text.find("host=127.0.0.1"));
    EXPECT_NE(std::string::npos, text.find("db=cloud_media"));
    EXPECT_NE(std::string::npos, text.find("pool total=4"));
    EXPECT_NE(std::string::npos, text.find("slow=2"));
}

TEST(MysqlErrorTest, ClassifiesConnectionErrors) {
    EXPECT_EQ(cm::ErrorCode::DbUnavailable, cm::db::classifyMysqlError(2003));
    EXPECT_EQ(cm::ErrorCode::DbUnavailable, cm::db::classifyMysqlError(2006));
    EXPECT_EQ(cm::ErrorCode::DbUnavailable, cm::db::classifyMysqlError(2013));
}

TEST(MysqlErrorTest, ClassifiesDeadlockAndTimeout) {
    EXPECT_EQ(cm::ErrorCode::DbTxConflict, cm::db::classifyMysqlError(1205));
    EXPECT_EQ(cm::ErrorCode::DbTxConflict, cm::db::classifyMysqlError(1213));
}

TEST(MysqlErrorTest, ClassifiesDuplicateKey) {
    EXPECT_EQ(cm::ErrorCode::AlreadyExists, cm::db::classifyMysqlError(1062));
    EXPECT_EQ(cm::ErrorCode::DbQueryFailed, cm::db::classifyMysqlError(9999));
}

TEST(SqlBuilderTest, Placeholders) {
    EXPECT_TRUE(cm::db::PreparedStatement::placeholders(0).empty());
    EXPECT_EQ(std::string("?"), cm::db::PreparedStatement::placeholders(1));
    EXPECT_EQ(std::string("?,?,?"), cm::db::PreparedStatement::placeholders(3));
}

TEST(SqlBuilderTest, BuildInsertUsesPlaceholders) {
    std::vector<std::string> columns;
    columns.push_back("f_md5");
    columns.push_back("f_size");

    EXPECT_EQ(std::string("INSERT INTO t_file (f_md5,f_size) VALUES (?,?)"),
              cm::db::PreparedStatement::buildInsert("t_file", columns));

    std::vector<std::string> badColumn;
    badColumn.push_back("f_md5; DROP TABLE t_file");
    EXPECT_TRUE(cm::db::PreparedStatement::buildInsert("t_file", badColumn).empty());
    EXPECT_TRUE(cm::db::PreparedStatement::buildInsert("t_file; DROP", columns).empty());
    EXPECT_TRUE(cm::db::PreparedStatement::buildInsert(
                    "t_file", std::vector<std::string>()).empty());
}

TEST(SlowQueryTest, ThresholdIsInclusive) {
    EXPECT_FALSE(cm::db::isSlowQuery(std::chrono::microseconds(999),
                                     std::chrono::milliseconds(1)));
    EXPECT_TRUE(cm::db::isSlowQuery(std::chrono::microseconds(1000),
                                    std::chrono::milliseconds(1)));
    EXPECT_FALSE(cm::db::isSlowQuery(std::chrono::microseconds(0),
                                     std::chrono::milliseconds(200)));
}

TEST(SlowQueryTest, FormatContainsNoParameters) {
    cm::db::SlowQueryInfo info;
    info.sql = "SELECT f_id FROM t_file WHERE f_md5=?";
    info.elapsed = std::chrono::microseconds(1500);
    info.rows = 2;
    info.inTransaction = false;

    const std::string text = cm::db::formatSlowQuery(info);
    EXPECT_NE(std::string::npos, text.find("SELECT f_id FROM t_file WHERE f_md5=?"));
    EXPECT_NE(std::string::npos, text.find("cost_ms=1"));
    EXPECT_NE(std::string::npos, text.find("rows=2"));
    EXPECT_NE(std::string::npos, text.find("in_tx=0"));
}

TEST(StorageErrorCodeTest, ValuesAreStable) {
    EXPECT_EQ(4001, static_cast<int>(cm::ErrorCode::DbUnavailable));
    EXPECT_EQ(4002, static_cast<int>(cm::ErrorCode::DbPoolTimeout));
    EXPECT_EQ(4003, static_cast<int>(cm::ErrorCode::DbQueryFailed));
    EXPECT_EQ(4004, static_cast<int>(cm::ErrorCode::DbTxConflict));
    EXPECT_EQ(4005, static_cast<int>(cm::ErrorCode::RedisUnavailable));
    EXPECT_EQ(4006, static_cast<int>(cm::ErrorCode::CacheRebuildFailed));
    EXPECT_EQ(4007, static_cast<int>(cm::ErrorCode::AuthUnavailable));
    EXPECT_EQ(4008, static_cast<int>(cm::ErrorCode::RateLimitUnavailable));

    EXPECT_STREQ("DbUnavailable",
                 cm::to_string(cm::ErrorCode::DbUnavailable));
    EXPECT_TRUE(cm::is_retryable(cm::ErrorCode::DbUnavailable));
    EXPECT_TRUE(cm::is_retryable(cm::ErrorCode::DbPoolTimeout));
    EXPECT_FALSE(cm::is_retryable(cm::ErrorCode::DbQueryFailed));
}

// ---------------------------------------------------------------------------
// 集成测试：未设置 CM_TEST_MYSQL 时跳过；设置后连不上必须 FAIL
// ---------------------------------------------------------------------------

class MysqlIntegration : public ::testing::Test {
protected:
    void SetUp() override {
        if (!envFlag("CM_TEST_MYSQL")) {
            GTEST_SKIP() << "set CM_TEST_MYSQL=1 to run MySQL integration tests";
        }
        std::string error;
        m_pool = cm::db::MysqlPool::create(integrationOptions(), &error);
        if (!m_pool) {
            FAIL() << "MySQL connection failed: " << error;
        }
    }

    std::shared_ptr<cm::db::MysqlPool> m_pool;
};

TEST_F(MysqlIntegration, ConnectAndPing) {
    std::unique_ptr<cm::db::MysqlConn> conn = m_pool->acquire();
    ASSERT_TRUE(conn) << static_cast<int>(m_pool->lastError());
    EXPECT_TRUE(conn->valid());
    EXPECT_TRUE(conn->ping());
    EXPECT_GT(conn->id(), std::uint64_t(0));
}

TEST_F(MysqlIntegration, PreparedRoundTrip) {
    std::unique_ptr<cm::db::MysqlConn> conn = m_pool->acquire();
    ASSERT_TRUE(conn) << static_cast<int>(m_pool->lastError());

    std::unique_ptr<cm::db::PreparedStatement> stmt = conn->prepare("SELECT ?");
    ASSERT_TRUE(stmt) << conn->lastErrorMessage();
    ASSERT_TRUE(stmt->bindString(0, "hello"));
    ASSERT_TRUE(stmt->execute()) << stmt->lastErrorMessage();
    ASSERT_TRUE(stmt->fetch()) << stmt->lastErrorMessage();
    EXPECT_EQ(std::string("hello"), stmt->getString(0));
    EXPECT_FALSE(stmt->fetch());
}

TEST_F(MysqlIntegration, TransactionRollbackOnDestruct) {
    std::unique_ptr<cm::db::MysqlConn> conn = m_pool->acquire();
    ASSERT_TRUE(conn) << static_cast<int>(m_pool->lastError());

    ASSERT_TRUE(conn->executeDdl("CREATE TEMPORARY TABLE cm_tx_probe (id INT)"))
        << conn->lastErrorMessage();

    {
        cm::db::Transaction tx(*conn);
        ASSERT_TRUE(tx.valid()) << conn->lastErrorMessage();
        std::unique_ptr<cm::db::PreparedStatement> insert =
            conn->prepare("INSERT INTO cm_tx_probe (id) VALUES (1)");
        ASSERT_TRUE(insert);
        ASSERT_TRUE(insert->execute()) << insert->lastErrorMessage();
    }

    {
        std::unique_ptr<cm::db::PreparedStatement> count =
            conn->prepare("SELECT COUNT(*) FROM cm_tx_probe");
        ASSERT_TRUE(count);
        ASSERT_TRUE(count->execute()) << count->lastErrorMessage();
        ASSERT_TRUE(count->fetch());
        EXPECT_EQ(std::int64_t(0), count->getInt64(0));
    }

    EXPECT_TRUE(conn->executeDdl("DROP TEMPORARY TABLE IF EXISTS cm_tx_probe"))
        << conn->lastErrorMessage();
}

TEST_F(MysqlIntegration, AcquireTimeoutReturnsNull) {
    cm::db::MysqlOptions options = integrationOptions();
    options.minSize = 1;
    options.maxSize = 1;
    options.acquireTimeout = std::chrono::milliseconds(50);

    std::string error;
    std::shared_ptr<cm::db::MysqlPool> pool =
        cm::db::MysqlPool::create(options, &error);
    ASSERT_TRUE(pool) << error;

    std::unique_ptr<cm::db::MysqlConn> first = pool->acquire();
    ASSERT_TRUE(first) << static_cast<int>(pool->lastError());

    std::unique_ptr<cm::db::MysqlConn> second =
        pool->acquire(std::chrono::milliseconds(50));
    EXPECT_FALSE(second);
    EXPECT_EQ(static_cast<int>(cm::ErrorCode::DbPoolTimeout),
              static_cast<int>(pool->lastError()));
    EXPECT_EQ(std::uint64_t(1), pool->stats().acquireTimeoutTotal);
}

TEST_F(MysqlIntegration, SlowQueryIsCounted) {
    cm::db::MysqlOptions options = integrationOptions();
    options.slowQueryThreshold = std::chrono::milliseconds(1);

    std::string error;
    std::shared_ptr<cm::db::MysqlPool> pool =
        cm::db::MysqlPool::create(options, &error);
    ASSERT_TRUE(pool) << error;

    std::unique_ptr<cm::db::MysqlConn> conn = pool->acquire();
    ASSERT_TRUE(conn) << static_cast<int>(pool->lastError());

    std::unique_ptr<cm::db::PreparedStatement> stmt =
        conn->prepare("SELECT SLEEP(0.05)");
    ASSERT_TRUE(stmt);
    ASSERT_TRUE(stmt->execute()) << stmt->lastErrorMessage();
    EXPECT_TRUE(stmt->fetch());
    EXPECT_FALSE(stmt->fetch());

    EXPECT_GE(pool->stats().slowQueryTotal, std::uint64_t(1));
}
