#pragma once

#include <cstddef>
#include <cstdint>

#include "cm/base/error_code.h"

namespace cm {
namespace db {
class MysqlPool;
}  // namespace db

namespace logic {

// 分享过期扫描：s_status=0 且 s_expire_at < NOW(6) 的置 1。
// 每日 03:10 由 ServerApp 触发；单轮上限由构造参数决定（默认 1000 行）。
class ShareExpiryJob {
public:
    explicit ShareExpiryJob(cm::db::MysqlPool& mysql,
                            std::size_t batch = 1000);

    ShareExpiryJob(const ShareExpiryJob&) = delete;
    ShareExpiryJob& operator=(const ShareExpiryJob&) = delete;

    cm::ErrorCode runOnce(std::uint64_t* affected);

private:
    cm::db::MysqlPool& m_mysql;
    std::size_t m_batch;
};

}  // namespace logic
}  // namespace cm
