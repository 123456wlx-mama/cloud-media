#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

#include "cm/base/md5.h"
#include "cm/base/sha256.h"
#include "cm/protocol/messages/auth.h"
#include "dao_fixture.h"
#include "db/user_dao.h"
#include "logic/auth_service.h"

namespace {

class AuthServiceTest : public cm::test::DaoTestBase {
protected:
    void SetUp() override {
        cm::test::DaoTestBase::SetUp();
        if (m_pool == nullptr) {
            return;  // 基类已 GTEST_SKIP（缺 MySQL）
        }
        m_auth.reset(new cm::logic::AuthService(*m_pool));
    }

    void TearDown() override {
        m_auth.reset();
        cm::test::DaoTestBase::TearDown();
    }

    cm::protocol::RegisterRq makeRegisterRq(const std::string& tel,
                                            const std::string& password,
                                            const std::string& name) {
        cm::protocol::RegisterRq rq;
        rq.tel = tel;
        rq.password = password;
        rq.name = name;
        return rq;
    }

    cm::protocol::LoginRq makeLoginRq(const std::string& tel,
                                      const std::string& password) {
        cm::protocol::LoginRq rq;
        rq.tel = tel;
        rq.password = password;
        return rq;
    }

    std::uint64_t registerOk(const std::string& tel,
                             const std::string& password,
                             const std::string& name) {
        cm::protocol::RegisterRs rs;
        EXPECT_EQ(cm::ErrorCode::Ok,
                  m_auth->registerUser(makeRegisterRq(tel, password, name), &rs));
        EXPECT_EQ(std::uint32_t(0), rs.code);
        if (rs.uid != 0) {
            trackUser(rs.uid);
        }
        return rs.uid;
    }

    std::unique_ptr<cm::logic::AuthService> m_auth;
};

TEST_F(AuthServiceTest, RegisterThenLogin) {
    const std::string tel = nextTel();
    const std::uint64_t uid = registerOk(tel, "secret123", "alice");
    ASSERT_GT(uid, std::uint64_t(0));

    // 存储格式：algo=2、salt 32 hex、hash = sha256Hex(salt + password)。
    cm::db::UserRow row;
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::db::UserDao(*m_conn).findByTel(tel, &row));
    EXPECT_EQ(uid, row.uid);
    EXPECT_EQ(std::string("alice"), row.name);
    EXPECT_EQ(std::uint8_t(2), row.pwdAlgo);
    EXPECT_EQ(std::size_t(32), row.salt.size());
    EXPECT_EQ(std::size_t(64), row.passwordHash.size());
    EXPECT_EQ(cm::sha256Hex(row.salt + "secret123"), row.passwordHash);
    EXPECT_NE(std::string("secret123"), row.passwordHash);

    cm::protocol::LoginRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_auth->login(makeLoginRq(tel, "secret123"), &rs));
    EXPECT_EQ(std::uint32_t(0), rs.code);
    EXPECT_EQ(uid, rs.uid);
    EXPECT_EQ(std::string("alice"), rs.name);
}

// 历史数据是 algo=1（MD5）时，登录必须放行，并在成功后自动升级为 algo=2。
TEST_F(AuthServiceTest, LegacyMd5UpgradedOnLogin) {
    const std::string tel = nextTel();
    const std::string salt(32, 'a');
    const std::string password = "legacy123";

    cm::db::UserDao dao(*m_conn);
    std::uint64_t uid = 0;
    ASSERT_EQ(cm::ErrorCode::Ok,
              dao.insert(tel, cm::md5Hex(salt + password), salt, 1, "legacy", &uid));
    ASSERT_GT(uid, std::uint64_t(0));
    trackUser(uid);

    cm::protocol::LoginRs rs;
    ASSERT_EQ(cm::ErrorCode::Ok, m_auth->login(makeLoginRq(tel, password), &rs));
    EXPECT_EQ(std::uint32_t(0), rs.code);
    EXPECT_EQ(uid, rs.uid);

    cm::db::UserRow after;
    ASSERT_EQ(cm::ErrorCode::Ok, dao.findByTel(tel, &after));
    EXPECT_EQ(std::uint8_t(2), after.pwdAlgo);
    EXPECT_EQ(salt, after.salt);  // 升级沿用原盐
    EXPECT_EQ(cm::sha256Hex(salt + password), after.passwordHash);
}

TEST_F(AuthServiceTest, DuplicateTelAlreadyExists) {
    const std::string tel = nextTel();
    ASSERT_GT(registerOk(tel, "pwd-one", "first"), std::uint64_t(0));

    cm::protocol::RegisterRs rs;
    EXPECT_EQ(cm::ErrorCode::AlreadyExists,
              m_auth->registerUser(makeRegisterRq(tel, "pwd-two", "second"),
                                   &rs));
    EXPECT_EQ(std::uint32_t(1003), rs.code);
    EXPECT_EQ(std::uint64_t(0), rs.uid);
}

TEST_F(AuthServiceTest, WrongPasswordNotFound) {
    const std::string tel = nextTel();
    ASSERT_GT(registerOk(tel, "correct", "bob"), std::uint64_t(0));

    cm::protocol::LoginRs rs;
    EXPECT_EQ(cm::ErrorCode::NotFound,
              m_auth->login(makeLoginRq(tel, "wrong"), &rs));
    EXPECT_EQ(std::uint32_t(1002), rs.code);
    EXPECT_EQ(std::uint64_t(0), rs.uid);
    EXPECT_TRUE(rs.name.empty());
}

TEST_F(AuthServiceTest, UnregisteredTelNotFound) {
    cm::protocol::LoginRs rs;
    EXPECT_EQ(cm::ErrorCode::NotFound,
              m_auth->login(makeLoginRq(nextTel(), "whatever"), &rs));
    EXPECT_EQ(std::uint32_t(1002), rs.code);
    EXPECT_EQ(std::uint64_t(0), rs.uid);
}

TEST_F(AuthServiceTest, PasswordNotEchoed) {
    const std::string tel = nextTel();
    const std::uint64_t uid = registerOk(tel, "no-echo-pwd", "carol");
    ASSERT_GT(uid, std::uint64_t(0));

    cm::db::UserRow row;
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::db::UserDao(*m_conn).findByTel(tel, &row));

    cm::protocol::RegisterRs registerRs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_auth->registerUser(makeRegisterRq(nextTel(), "other-pwd", "d"),
                                   &registerRs));
    std::string registerPayload;
    ASSERT_TRUE(registerRs.encode(&registerPayload));
    EXPECT_EQ(std::string::npos,
              registerPayload.find(std::string("other-pwd")));

    cm::protocol::LoginRs loginRs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              m_auth->login(makeLoginRq(tel, "no-echo-pwd"), &loginRs));
    std::string loginPayload;
    ASSERT_TRUE(loginRs.encode(&loginPayload));
    EXPECT_EQ(std::string::npos, loginPayload.find(std::string("no-echo-pwd")));
    EXPECT_EQ(std::string::npos, loginPayload.find(row.passwordHash));
    EXPECT_EQ(std::string::npos, loginPayload.find(row.salt));
    EXPECT_EQ(std::string::npos, loginPayload.find(std::string("salt")));
}

}  // namespace
