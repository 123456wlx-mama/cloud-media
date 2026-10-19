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

// 下载服务：10010 打开、10027 续传校验、10040 完成上报。
// ticket 只写 Redis，不落 MySQL；内容与续期语义由 DownloadTicket 统一维护。
class DownloadService {
public:
    DownloadService(cm::db::MysqlPool& mysql,
                    cm::cache::RedisPool& redis,
                    const std::string& dataDir);

    DownloadService(const DownloadService&) = delete;
    DownloadService& operator=(const DownloadService&) = delete;

    // 10010：校验本人节点 → 校验 blob 就绪 → 签发 ticket。
    cm::ErrorCode openDownload(std::uint64_t uid,
                               const cm::protocol::DownloadOpenRq& rq,
                               cm::protocol::DownloadOpenRs* out);

    // 10027：返回远端元信息（size/md5/block_size/block_count），供客户端校验 .part。
    cm::ErrorCode stat(std::uint64_t uid,
                       const cm::protocol::DownloadStatRq& rq,
                       cm::protocol::DownloadStatRs* out);

    // 10040：完成上报。Redis 去重 10min，不写 DB；verify_ok=0 仅告警。
    cm::ErrorCode done(std::uint64_t uid,
                       const cm::protocol::DownloadDoneRq& rq,
                       cm::protocol::DownloadDoneRs* out);

private:
    cm::db::MysqlPool& m_mysql;
    cm::cache::RedisPool& m_redis;
    DownloadTicket m_ticket;
    std::string m_dataDir;
};

}  // namespace logic
}  // namespace cm
