#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

#include "db/mysql_pool.h"
#include "db/prepared_statement.h"

namespace {

bool mysqlEnabled() {
    const char* value = std::getenv("CM_TEST_MYSQL");
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
    options.minSize = 1;
    options.maxSize = 2;
    return options;
}

class SchemaV22Test : public ::testing::Test {
protected:
    void SetUp() override {
        if (!mysqlEnabled()) {
            GTEST_SKIP() << "set CM_TEST_MYSQL=1 to run schema contract tests";
        }
        m_options = mysqlOptions();
        std::string error;
        m_pool = cm::db::MysqlPool::create(m_options, &error);
        ASSERT_TRUE(m_pool) << error;
        m_conn = m_pool->acquire();
        ASSERT_TRUE(m_conn);
    }

    void TearDown() override {
        m_conn.reset();
        if (m_pool) {
            m_pool->shutdown();
            m_pool.reset();
        }
    }

    std::unique_ptr<cm::db::PreparedStatement> prepare(const std::string& sql) {
        std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(sql);
        EXPECT_TRUE(stmt) << m_conn->lastErrorMessage();
        return stmt;
    }

    cm::db::MysqlOptions m_options;
    std::shared_ptr<cm::db::MysqlPool> m_pool;
    std::unique_ptr<cm::db::MysqlConn> m_conn;
};

}  // namespace

TEST_F(SchemaV22Test, ShareCodeIsSixAsciiChars) {
    std::unique_ptr<cm::db::PreparedStatement> stmt = prepare(
        "SELECT CHARACTER_MAXIMUM_LENGTH, CHARACTER_SET_NAME, COLUMN_COMMENT "
        "FROM information_schema.COLUMNS "
        "WHERE TABLE_SCHEMA = ? AND TABLE_NAME = 't_share' "
        "AND COLUMN_NAME = 's_code'");
    ASSERT_TRUE(stmt);
    ASSERT_TRUE(stmt->bindString(0, m_options.database));
    ASSERT_TRUE(stmt->execute());
    ASSERT_EQ(cm::db::PreparedStatement::FetchStatus::Row, stmt->fetchRow());

    EXPECT_EQ(std::uint64_t(6), stmt->getUInt64(0));
    EXPECT_EQ(std::string("ascii"), stmt->getString(1));
    EXPECT_NE(std::string::npos, stmt->getString(2).find("6位分享码"));
}

TEST_F(SchemaV22Test, UserFileHasIdxDeleted) {
    std::unique_ptr<cm::db::PreparedStatement> stmt = prepare(
        "SELECT SEQ_IN_INDEX, COLUMN_NAME "
        "FROM information_schema.STATISTICS "
        "WHERE TABLE_SCHEMA = ? AND TABLE_NAME = 't_user_file' "
        "AND INDEX_NAME = 'idx_deleted' ORDER BY SEQ_IN_INDEX");
    ASSERT_TRUE(stmt);
    ASSERT_TRUE(stmt->bindString(0, m_options.database));
    ASSERT_TRUE(stmt->execute());

    ASSERT_EQ(cm::db::PreparedStatement::FetchStatus::Row, stmt->fetchRow());
    EXPECT_EQ(std::uint64_t(1), stmt->getUInt64(0));
    EXPECT_EQ(std::string("u_id"), stmt->getString(1));

    ASSERT_EQ(cm::db::PreparedStatement::FetchStatus::Row, stmt->fetchRow());
    EXPECT_EQ(std::uint64_t(2), stmt->getUInt64(0));
    EXPECT_EQ(std::string("uf_deleted_at"), stmt->getString(1));

    EXPECT_EQ(cm::db::PreparedStatement::FetchStatus::End,
              stmt->fetchRow());
}

TEST_F(SchemaV22Test, TransferStatusCommentHasMerging) {
    std::unique_ptr<cm::db::PreparedStatement> stmt = prepare(
        "SELECT COLUMN_COMMENT FROM information_schema.COLUMNS "
        "WHERE TABLE_SCHEMA = ? AND TABLE_NAME = 't_transfer_task' "
        "AND COLUMN_NAME = 'tr_status'");
    ASSERT_TRUE(stmt);
    ASSERT_TRUE(stmt->bindString(0, m_options.database));
    ASSERT_TRUE(stmt->execute());
    ASSERT_EQ(cm::db::PreparedStatement::FetchStatus::Row, stmt->fetchRow());
    EXPECT_NE(std::string::npos, stmt->getString(0).find("7合并中"));
}

TEST_F(SchemaV22Test, TransferOffsetCommentHasDoneBytes) {
    std::unique_ptr<cm::db::PreparedStatement> stmt = prepare(
        "SELECT COLUMN_COMMENT FROM information_schema.COLUMNS "
        "WHERE TABLE_SCHEMA = ? AND TABLE_NAME = 't_transfer_task' "
        "AND COLUMN_NAME = 'tr_offset'");
    ASSERT_TRUE(stmt);
    ASSERT_TRUE(stmt->bindString(0, m_options.database));
    ASSERT_TRUE(stmt->execute());
    ASSERT_EQ(cm::db::PreparedStatement::FetchStatus::Row, stmt->fetchRow());
    EXPECT_NE(std::string::npos,
              stmt->getString(0).find("已完成分片字节总和"));
}

TEST_F(SchemaV22Test, NoForeignKeysOrTriggers) {
    {
        std::unique_ptr<cm::db::PreparedStatement> stmt = prepare(
            "SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS "
            "WHERE TABLE_SCHEMA = ? AND CONSTRAINT_TYPE = 'FOREIGN KEY'");
        ASSERT_TRUE(stmt);
        ASSERT_TRUE(stmt->bindString(0, m_options.database));
        ASSERT_TRUE(stmt->execute());
        ASSERT_EQ(cm::db::PreparedStatement::FetchStatus::Row,
                  stmt->fetchRow());
        EXPECT_EQ(std::uint64_t(0), stmt->getUInt64(0));
    }
    {
        std::unique_ptr<cm::db::PreparedStatement> stmt = prepare(
            "SELECT COUNT(*) FROM information_schema.TRIGGERS "
            "WHERE TRIGGER_SCHEMA = ?");
        ASSERT_TRUE(stmt);
        ASSERT_TRUE(stmt->bindString(0, m_options.database));
        ASSERT_TRUE(stmt->execute());
        ASSERT_EQ(cm::db::PreparedStatement::FetchStatus::Row,
                  stmt->fetchRow());
        EXPECT_EQ(std::uint64_t(0), stmt->getUInt64(0));
    }
}
