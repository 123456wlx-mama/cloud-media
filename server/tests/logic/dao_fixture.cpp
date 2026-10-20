#include "dao_fixture.h"

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "db/file_dao.h"
#include "db/prepared_statement.h"
#include "db/share_dao.h"
#include "db/transfer_dao.h"

namespace cm {
namespace test {
namespace {

const char kPasswordHash[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";  // 64
const char kSalt[] = "0123456789abcdef0123456789abcdef";                  // 32
const std::uint32_t kChunkSize = 4194304;

std::atomic<int> g_instanceSeq(0);

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

}  // namespace

void DaoTestBase::SetUp() {
    if (!mysqlEnabled()) {
        GTEST_SKIP() << "set CM_TEST_MYSQL=1 to run DAO integration tests";
        return;
    }

    const unsigned int sequence =
        static_cast<unsigned int>(g_instanceSeq.fetch_add(1)) % 100U;
    m_seed = static_cast<std::uint64_t>(sequence);

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "cm%03ld%06d%02d",
                  static_cast<long>(::getpid()) % 1000L,
                  static_cast<int>(std::time(nullptr)) % 1000000,
                  static_cast<int>(sequence));
    m_prefix = std::string(buffer);

    std::string error;
    m_pool = cm::db::MysqlPool::create(mysqlOptions(), &error);
    ASSERT_TRUE(m_pool) << error;
    m_conn = m_pool->acquire();
    ASSERT_TRUE(m_conn) << "acquire MySQL connection failed";
}

void DaoTestBase::TearDown() {
    cleanup();
    m_conn.reset();
    if (m_pool) {
        m_pool->shutdown();
        m_pool.reset();
    }
}

std::string DaoTestBase::nextTel() {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%s%03d", m_prefix.c_str(), m_seq++ % 1000);
    return std::string(buffer);
}

std::string DaoTestBase::nextHex32() {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%08x%08x%08x%08x",
                  static_cast<unsigned int>(::getpid()) & 0xffffffffU,
                  static_cast<unsigned int>(std::time(nullptr)) & 0xffffffffU,
                  static_cast<unsigned int>(m_seed) & 0xffffffffU,
                  static_cast<unsigned int>(m_seq++ * 7919U + 0x5a5a0000U) &
                      0xffffffffU);
    return std::string(buffer);
}

std::string DaoTestBase::nextCode() {
    const char* alphabet = cm::db::ShareDao::codeAlphabet();
    const std::size_t base = std::string(alphabet).size();

    unsigned long long value =
        static_cast<unsigned long long>(::getpid()) * 2654435761ULL +
        static_cast<unsigned long long>(std::time(nullptr)) * 40503ULL +
        m_seed * 104729ULL + static_cast<unsigned long long>(m_seq++) * 1000003ULL;

    std::string code(6, alphabet[0]);
    for (std::size_t i = 0; i < code.size(); ++i) {
        code[i] = alphabet[value % base];
        value /= base;
    }
    return code;
}

std::uint64_t DaoTestBase::createUser() {
    if (!m_conn) {
        return 0;
    }
    const std::string tel = nextTel();

    std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(
        "INSERT INTO t_user (u_tel, u_password, u_salt, u_pwd_algo, u_name) "
        "VALUES (?, ?, ?, 2, 'dao_fixture')");
    if (!stmt || !stmt->bindString(0, tel) || !stmt->bindString(1, kPasswordHash) ||
        !stmt->bindString(2, kSalt) || !stmt->execute()) {
        return 0;
    }

    const std::uint64_t uid = stmt->insertId();
    m_uid = uid;
    trackUser(uid);
    return uid;
}

std::uint64_t DaoTestBase::createBlob(std::uint64_t size) {
    if (!m_conn) {
        return 0;
    }
    cm::db::FileDao dao(*m_conn);

    const std::string md5 = nextHex32();
    std::uint64_t fid = 0;
    cm::db::BlobWriteKind kind = cm::db::BlobWriteKind::Inserted;
    if (dao.insertBlobOnDuplicateIncrement(md5, size, "aa/bb/" + md5, &fid,
                                           &kind) != cm::ErrorCode::Ok) {
        return 0;
    }
    trackBlob(fid);
    return fid;
}

std::uint64_t DaoTestBase::createNode(std::uint64_t uid,
                                      const std::string& name,
                                      std::uint64_t fileId,
                                      std::string* finalName) {
    if (!m_conn || uid == 0) {
        return 0;
    }
    cm::db::FileDao dao(*m_conn);

    const bool isDir = (fileId == 0);
    std::uint64_t ufId = 0;
    std::string resolved;
    if (dao.createUserFileAutoRename(uid, 0, fileId, isDir, name, &ufId,
                                     &resolved) != cm::ErrorCode::Ok) {
        return 0;
    }
    trackNode(ufId);
    if (finalName != nullptr) {
        *finalName = resolved;
    }
    return ufId;
}

std::string DaoTestBase::createTask(std::uint64_t uid,
                                   std::uint32_t totalChunks,
                                   std::uint64_t totalSize) {
    if (!m_conn || uid == 0) {
        return std::string();
    }
    cm::db::TransferDao dao(*m_conn);

    cm::db::NewTransferTask task;
    task.taskId = nextHex32();
    task.userId = uid;
    task.type = 1;
    task.fileMd5 = nextHex32();
    task.fileName = "phase5.bin";
    task.totalSize = totalSize;
    task.chunkSize = kChunkSize;
    task.totalChunks = totalChunks;
    task.parentUfId = 0;
    task.ttlSeconds = 3600;

    std::uint64_t trId = 0;
    if (dao.createTask(task, &trId) != cm::ErrorCode::Ok) {
        return std::string();
    }
    trackTask(task.taskId);
    return task.taskId;
}

std::string DaoTestBase::createShare(std::uint64_t uid,
                                    std::uint64_t ufId,
                                    bool isDir,
                                    std::uint64_t fileId,
                                    const std::string& accessCode,
                                    std::uint8_t expirePolicy) {
    if (!m_conn || uid == 0) {
        return std::string();
    }
    cm::db::ShareDao dao(*m_conn);

    cm::db::NewShare share;
    share.code = nextCode();
    share.userId = uid;
    share.ufId = ufId;
    share.fileId = fileId;
    share.isDir = isDir;
    share.accessCode = accessCode;
    share.expirePolicy = expirePolicy;

    std::uint64_t shareId = 0;
    if (dao.insert(share, &shareId) != cm::ErrorCode::Ok) {
        return std::string();
    }
    trackShare(share.code);
    return share.code;
}

void DaoTestBase::track(std::vector<std::uint64_t>* list, std::uint64_t value) {
    if (value != 0) {
        list->push_back(value);
    }
}

void DaoTestBase::trackUser(std::uint64_t uid) {
    track(&m_uids, uid);
}

void DaoTestBase::trackBlob(std::uint64_t fid) {
    track(&m_fids, fid);
}

void DaoTestBase::trackNode(std::uint64_t ufId) {
    track(&m_ufIds, ufId);
}

void DaoTestBase::trackTask(const std::string& taskId) {
    if (!taskId.empty()) {
        m_taskIds.push_back(taskId);
    }
}

void DaoTestBase::trackShare(const std::string& code) {
    if (!code.empty()) {
        m_shareCodes.push_back(code);
    }
}

bool DaoTestBase::execPrepared(const char* sql, std::uint64_t value) {
    if (!m_conn) {
        return false;
    }
    std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(sql);
    if (!stmt || !stmt->bindUInt64(0, value)) {
        return false;
    }
    return stmt->execute();
}

bool DaoTestBase::execPreparedString(const char* sql, const std::string& value) {
    if (!m_conn) {
        return false;
    }
    std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(sql);
    if (!stmt || !stmt->bindString(0, value)) {
        return false;
    }
    return stmt->execute();
}

bool DaoTestBase::execPreparedString(const char* sql,
                                     const std::string& first,
                                     std::uint64_t second) {
    if (!m_conn) {
        return false;
    }
    std::unique_ptr<cm::db::PreparedStatement> stmt = m_conn->prepare(sql);
    if (!stmt || !stmt->bindString(0, first) || !stmt->bindUInt64(1, second)) {
        return false;
    }
    return stmt->execute();
}

void DaoTestBase::cleanup() {
    if (!m_conn) {
        return;
    }

    // 固定顺序：t_chunk → t_transfer_task → t_share → t_play_record →
    //           t_user_file → t_file → t_user
    for (std::size_t i = 0; i < m_taskIds.size(); ++i) {
        execPreparedString("DELETE FROM t_chunk WHERE c_task_id = ?", m_taskIds[i]);
    }
    for (std::size_t i = 0; i < m_taskIds.size(); ++i) {
        execPreparedString("DELETE FROM t_transfer_task WHERE tr_task_id = ?",
                           m_taskIds[i]);
    }
    for (std::size_t i = 0; i < m_uids.size(); ++i) {
        execPrepared("DELETE FROM t_transfer_task WHERE tr_user_id = ?", m_uids[i]);
    }
    for (std::size_t i = 0; i < m_shareCodes.size(); ++i) {
        execPreparedString("DELETE FROM t_share WHERE s_code = ?", m_shareCodes[i]);
    }
    for (std::size_t i = 0; i < m_uids.size(); ++i) {
        execPrepared("DELETE FROM t_share WHERE s_user_id = ?", m_uids[i]);
    }
    for (std::size_t i = 0; i < m_uids.size(); ++i) {
        execPrepared("DELETE FROM t_play_record WHERE p_user_id = ?", m_uids[i]);
    }
    for (std::size_t i = 0; i < m_uids.size(); ++i) {
        execPrepared("DELETE FROM t_user_file WHERE u_id = ?", m_uids[i]);
    }
    for (std::size_t i = 0; i < m_ufIds.size(); ++i) {
        execPrepared("DELETE FROM t_user_file WHERE uf_id = ?", m_ufIds[i]);
    }
    for (std::size_t i = 0; i < m_fids.size(); ++i) {
        execPrepared("DELETE FROM t_file WHERE f_id = ?", m_fids[i]);
    }
    for (std::size_t i = 0; i < m_uids.size(); ++i) {
        execPrepared("DELETE FROM t_user WHERE u_id = ?", m_uids[i]);
    }
    // UserDao 用例直接用 DAO 插用户，只能按 tel 前缀兜底。
    if (!m_prefix.empty()) {
        execPreparedString("DELETE FROM t_user WHERE u_tel LIKE ?", m_prefix + "%");
    }

    m_uids.clear();
    m_fids.clear();
    m_ufIds.clear();
    m_taskIds.clear();
    m_shareCodes.clear();
}

}  // namespace test
}  // namespace cm
