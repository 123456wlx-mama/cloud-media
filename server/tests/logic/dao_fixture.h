#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "db/mysql_conn.h"
#include "db/mysql_pool.h"

namespace cm {
namespace test {

// DAO 集成测试共用的基类：
//  - SetUp 检查 CM_TEST_MYSQL，未设置时 GTEST_SKIP 并让 m_pool 保持空；
//  - 提供带唯一前缀的 tel / md5 / 分享码生成器；
//  - 提供 createUser / createBlob / createNode / createTask / createShare，返回主键并登记清理；
//  - TearDown 按登记 ID 与前缀清理，禁止 TRUNCATE。
//
// 派生类 SetUp 的写法：
//   DaoTestBase::SetUp();
//   if (m_pool == NULL) { return; }   // 基类已 GTEST_SKIP
//   m_dao.reset(new XxxDao(*m_conn));
class DaoTestBase : public ::testing::Test {
protected:
    void SetUp() override;
    void TearDown() override;

    // pid + 时间 + 进程内序号；同一进程内每个用例实例都不同。
    const std::string& prefix() const { return m_prefix; }

    std::string nextTel();    // <= 20 字节，用于 t_user.u_tel
    std::string nextHex32();  // 32 位小写 hex：md5 / task_id / file_md5
    std::string nextCode();   // 6 位分享码（23456789ABCDEFGHJKMNPQRSTUVWXYZ）

    std::uint64_t createUser();  // 返回 u_id，同时写入 m_uid
    std::uint64_t createBlob(std::uint64_t size);
    // fileId=0 表示目录（f_id 写 NULL）；finalName 回填自动改名后的名字。
    std::uint64_t createNode(std::uint64_t uid,
                             const std::string& name,
                             std::uint64_t fileId,
                             std::string* finalName = nullptr);
    // 返回 task_id（对外键，32 hex）；tr_id 是内部自增主键，用例不需要。
    std::string createTask(std::uint64_t uid,
                           std::uint32_t totalChunks,
                           std::uint64_t totalSize);
    // 返回 share_code（对外键，6 位）；s_id 是内部自增主键。
    std::string createShare(std::uint64_t uid,
                            std::uint64_t ufId,
                            bool isDir,
                            std::uint64_t fileId,
                            const std::string& accessCode,
                            std::uint8_t expirePolicy);

    // 用例直接调用 DAO 写入时，需要自己登记，否则 TearDown 清不掉。
    void trackUser(std::uint64_t uid);
    void trackBlob(std::uint64_t fid);
    void trackNode(std::uint64_t ufId);
    void trackTask(const std::string& taskId);
    void trackShare(const std::string& code);

    // 单参数预处理语句，供用例改状态（过期、状态位、同一微秒等）。
    bool execPrepared(const char* sql, std::uint64_t value);
    bool execPreparedString(const char* sql, const std::string& value);
    // 双参数版本（字符串 + 整数），用于 UPDATE ... SET col = ? WHERE uid = ?。
    bool execPreparedString(const char* sql,
                            const std::string& first,
                            std::uint64_t second);

    // 幂等；TearDown 会再调一次。
    void cleanup();

    std::shared_ptr<cm::db::MysqlPool> m_pool;
    std::unique_ptr<cm::db::MysqlConn> m_conn;
    std::uint64_t m_uid = 0;
    int m_seq = 0;

private:
    void track(std::vector<std::uint64_t>* list, std::uint64_t value);

    std::string m_prefix;
    std::uint64_t m_seed = 0;
    std::vector<std::uint64_t> m_uids;
    std::vector<std::uint64_t> m_fids;
    std::vector<std::uint64_t> m_ufIds;
    std::vector<std::string> m_taskIds;
    std::vector<std::string> m_shareCodes;
};

}  // namespace test
}  // namespace cm
