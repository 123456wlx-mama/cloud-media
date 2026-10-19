#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "db/mysql_pool.h"
#include "db/prepared_statement.h"
#include "db/user_dao.h"

namespace {

const char* kHashA =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";  // 64
const char* kHashB =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";  // 64
const char* kSalt32 = "0123456789abcdef0123456789abcdef";               // 32

bool mysqlEnabled() {
    const char* value = std::getenv("CM_TEST_MYSQL");
    return value != NULL && std::string(value) == "1";
}

std::string envOr(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != NULL ? std::string(value) : std::string(fallback);
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

// u_tel 为 VARCHAR(20) ascii：pid + 时间低位 + 序号，保证同机重复跑不撞唯一键。
std::string makeTel(int seq) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "cmt%ld%02d%03d",
                  static_cast<long>(::getpid()) % 100000L,
                  static_cast<int>(std::time(NULL)) % 100, seq % 1000);
    return std::string(buffer);
}

class UserDaoTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!mysqlEnabled()) {
            GTEST_SKIP() << "set CM_TEST_MYSQL=1 to run UserDao tests";
        }
        m_options = mysqlOptions();
        std::string error;
        m_pool = cm::db::MysqlPool::create(m_options, &error);
        ASSERT_TRUE(m_pool) << error;
        m_conn = m_pool->acquire();
        ASSERT_TRUE(m_conn) << "acquire MySQL connection failed";
        m_dao.reset(new cm::db::UserDao(*m_conn));
    }

    void TearDown() override {
        if (m_conn) {
            for (std::size_t i = 0; i < m_tels.size(); ++i) {
                std::unique_ptr<cm::db::PreparedStatement> stmt =
                    m_conn->prepare("DELETE FROM t_user WHERE u_tel = ?");
                if (!stmt) {
                    continue;
                }
                if (!stmt->bindString(0, m_tels[i])) {
                    continue;
                }
                stmt->execute();
            }
        }
        m_dao.reset();
        m_conn.reset();
        if (m_pool) {
            m_pool->shutdown();
            m_pool.reset();
        }
    }

    std::string nextTel() {
        static int seq = 0;
        const std::string tel = makeTel(seq++);
        m_tels.push_back(tel);
        return tel;
    }

    cm::db::MysqlOptions m_options;
    std::shared_ptr<cm::db::MysqlPool> m_pool;
    std::unique_ptr<cm::db::MysqlConn> m_conn;
    std::unique_ptr<cm::db::UserDao> m_dao;
    std::vector<std::string> m_tels;
};

TEST_F(UserDaoTest, UserDaoFindMissing) {
    cm::db::UserRow row;
    EXPECT_EQ(cm::ErrorCode::NotFound, m_dao->findByTel(nextTel(), &row));
}

TEST_F(UserDaoTest, UserDaoInsertAndFind) {
    const std::string tel = nextTel();
    std::uint64_t uid = 0;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->insert(tel, kHashA, kSalt32, 2, "dao_test", &uid));
    ASSERT_GT(uid, std::uint64_t(0));

    cm::db::UserRow row;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findByTel(tel, &row));
    EXPECT_EQ(uid, row.uid);
    EXPECT_EQ(tel, row.tel);
    EXPECT_EQ(std::string(kHashA), row.passwordHash);
    EXPECT_EQ(std::string(kSalt32), row.salt);
    EXPECT_EQ(2, static_cast<int>(row.pwdAlgo));
    EXPECT_EQ(std::string("dao_test"), row.name);
    EXPECT_EQ(0, static_cast<int>(row.status));
}

TEST_F(UserDaoTest, UserDaoDuplicateTel) {
    const std::string tel = nextTel();
    std::uint64_t uid = 0;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->insert(tel, kHashA, kSalt32, 2, "dup_first", &uid));

    std::uint64_t again = 0;
    EXPECT_EQ(cm::ErrorCode::AlreadyExists,
              m_dao->insert(tel, kHashB, kSalt32, 2, "dup_second", &again));
    EXPECT_EQ(std::uint64_t(0), again);
}

TEST_F(UserDaoTest, UserDaoUpdatePassword) {
    const std::string tel = nextTel();
    std::uint64_t uid = 0;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->insert(tel, kHashA, kSalt32, 2, "pwd_user", &uid));

    ASSERT_EQ(cm::ErrorCode::Ok,
              m_dao->updatePassword(uid, kHashB, 1, kSalt32));

    cm::db::UserRow row;
    ASSERT_EQ(cm::ErrorCode::Ok, m_dao->findByTel(tel, &row));
    EXPECT_EQ(std::string(kHashB), row.passwordHash);
    EXPECT_EQ(1, static_cast<int>(row.pwdAlgo));

    // 回归：同值再写一次，MySQL changed rows 为 0，必须仍返回 Ok 而不是 NotFound。
    EXPECT_EQ(cm::ErrorCode::Ok,
              m_dao->updatePassword(uid, kHashB, 1, kSalt32));

    // 不存在的 uid 才返回 NotFound。
    EXPECT_EQ(cm::ErrorCode::NotFound,
              m_dao->updatePassword(uid + 100000000ULL, kHashB, 1, kSalt32));
}

TEST_F(UserDaoTest, UserDaoRejectsBadArguments) {
    cm::db::UserRow row;
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->findByTel("", &row));
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->findByTel("x", NULL));

    std::uint64_t uid = 0;
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              m_dao->insert(nextTel(), kHashA, kSalt32, 9, "bad_algo", &uid));
}

}  // namespace
