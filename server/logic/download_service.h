#pragma once

#include <cstdint>
#include <string>

#include "cm/base/error_code.h"
#include "cm/protocol/messages/download.h"
#include "logic/download_ticket.h"

namespace cm {

namespace db {
class MysqlPool;
}  // namespace db

namespace cache {
class RedisPool;
}  // namespace cache

namespace logic {

// 10010 DownloadOpen：只认当前用户名下的文件节点（目录分享在 S-3 处理）。
// ticket 只写 Redis，不落 MySQL；内容与续期语义由 DownloadTicket 统一维护。
class DownloadService {
public:
    DownloadService(cm::db::MysqlPool& mysql,
                    cm::cache::RedisPool& redis,
                    const std::string& dataDir);

    DownloadService(const DownloadService&) = delete;
    DownloadService& operator=(const DownloadService&) = delete;

    cm::ErrorCode openDownload(std::uint64_t uid,
                               const cm::protocol::DownloadOpenRq& rq,
                               cm::protocol::DownloadOpenRs* out);

private:
    cm::db::MysqlPool& m_mysql;
    DownloadTicket m_ticket;
    std::string m_dataDir;
};

}  // namespace logic
}  // namespace cm
