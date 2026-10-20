#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

#include "dao_fixture.h"
#include "db/user_dao.h"

namespace {

const char* kHashA =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";  // 64
const char* kHashB =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";  // 64
const char* kSalt32 = "0123456789abcdef0123456789abcdef";               // 32

class UserDaoTest : public cm::test::DaoTestBase {
protected:
    void SetUp() override {
        cm::test::DaoTestBase::SetUp();
        if (m_pool == nullptr) {
            return;  // 基类已 GTEST_SKIP
        }
        m_dao.reset(new cm::db::UserDao(*m_conn));
    }

    std::unique_ptr<cm::db::UserDao> m_dao;
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
    EXPECT_EQ(cm::ErrorCode::InvalidArgument, m_dao->findByTel("x", nullptr));

    std::uint64_t uid = 0;
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              m_dao->insert(nextTel(), kHashA, kSalt32, 9, "bad_algo", &uid));
}

}  // namespace
