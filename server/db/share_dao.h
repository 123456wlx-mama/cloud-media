#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cm/base/error_code.h"

namespace cm {
namespace db {

class MysqlConn;

struct ShareRow {
    std::uint64_t shareId;
    std::string code;
    std::uint64_t userId;
    std::uint64_t ufId;
    std::uint64_t fileId;    // 0 = NULL（目录分享）
    std::string accessCode;  // 空 = NULL（无提取码）
    std::uint8_t status;     // 0有效 1取消
    std::string expireAt;    // DATETIME(6) 文本；永久为 9999 哨兵
    std::string createdAt;
    std::string updatedAt;

    ShareRow();
};

struct NewShare {
    std::string code;          // 6 位，字符表 23456789ABCDEFGHJKMNPQRSTUVWXYZ
    std::uint64_t userId;
    std::uint64_t ufId;
    std::uint64_t fileId;      // isDir=false 时必须 >0；isDir=true 时必须为 0
    bool isDir;
    std::string accessCode;    // 空串写 NULL；非空必须是 4 位数字
    std::uint8_t expirePolicy; // 0=永久 1=1 天 2=7 天（06 §4.11）

    NewShare();
};

struct ShareListQuery {
    std::uint64_t userId;
    std::size_t limit;
    std::string cursorCreatedAt;  // 上一页末行的 s_created_at 文本
    std::uint64_t cursorId;       // 上一页末行的 s_id

    ShareListQuery();
};

struct ShareListPage {
    std::vector<ShareRow> rows;
    bool hasMore;
    std::string nextCreatedAt;
    std::uint64_t nextId;

    ShareListPage();
};

// t_share 的 DAO。不持有连接池，调用方用 MysqlPool::acquire 取得连接。
// 所有 SQL 均为预处理语句；分享码冲突由服务层重试，DAO 只把 1062 映射成 AlreadyExists。
class ShareDao {
public:
    explicit ShareDao(MysqlConn& conn);

    ShareDao(const ShareDao&) = delete;
    ShareDao& operator=(const ShareDao&) = delete;

    // 31 字符表（去掉 0/O/1/I/L），与 06 §7.1 一致。
    static const char* codeAlphabet();
    static bool isValidCode(const std::string& code);

    // s_expire_at 由 expirePolicy 在 SQL 内按 NOW(6) 计算，避免客户端时区偏差。
    ErrorCode insert(const NewShare& share, std::uint64_t* shareId);

    ErrorCode findByCode(const std::string& code, ShareRow* out);

    // 只返回未取消且未过期的分享；keyset 游标 (s_created_at, s_id) < (?, ?)。
    ErrorCode listByUser(const ShareListQuery& query, ShareListPage* page);

    // 幂等：已取消返回 Ok + already=true；码不存在或不属于该用户返回 NotFound。
    ErrorCode cancel(std::uint64_t userId,
                     const std::string& code,
                     bool* cancelled,
                     bool* already);

    // 节点上仍然有效的分享码（用于删除/改名后失效缓存）。s_uf_id 全局唯一，无需 uid。
    ErrorCode listActiveByUfId(std::uint64_t ufId, std::vector<std::string>* codes);

    // 每日扫描：s_status=0 且 s_expire_at < NOW(6) 的置 1，返回受影响行数。
    ErrorCode sweepExpired(std::size_t limit, std::uint64_t* affected);

private:
    MysqlConn& m_conn;
};

}  // namespace db
}  // namespace cm
