#pragma once

#include <cstdint>

#include "cm/base/error_code.h"
#include "cm/protocol/messages/upload.h"

namespace cm {

namespace db {
class MysqlConn;
class MysqlPool;
}  // namespace db

namespace cache {
class CacheStore;
}  // namespace cache

namespace logic {

// 10004 UploadInit：参数校验 → req 幂等 → 秒传 → 建上传任务。
// 只允许在业务线程调用：内部会阻塞访问 MySQL 与 Redis。
// 依赖只通过 cm::db 的 DAO 与 cm::cache::CacheStore，不直接使用 MySQL/Redis C API。
class UploadService {
public:
    UploadService(cm::db::MysqlPool& mysql, cm::cache::CacheStore& cache);

    UploadService(const UploadService&) = delete;
    UploadService& operator=(const UploadService&) = delete;

    // 成功返回 Ok 且 out->code==0；业务失败时 out->code 为对应数值
    // （1001/1002/1003/1006/3001/4001~4005），调用方直接把 out 当 10005 回包。
    cm::ErrorCode init(std::uint64_t uid,
                       const cm::protocol::UploadInitRq& rq,
                       cm::protocol::UploadInitRs* out);

    // 10028：从 t_chunk 重建续传位图（LSB-first），顺带修正计数缓存。
    // 不存在与非本人一律 1004；位图长度固定 ceil(total_chunks/8)。
    cm::ErrorCode continueUpload(std::uint64_t uid,
                                 const cm::protocol::ContinueUploadRq& rq,
                                 cm::protocol::ContinueUploadRs* out);

private:
    cm::ErrorCode doContinue(cm::db::MysqlConn& conn,
                             std::uint64_t uid,
                             const cm::protocol::ContinueUploadRq& rq,
                             cm::protocol::ContinueUploadRs* out);

    cm::ErrorCode doInit(cm::db::MysqlConn& conn,
                         std::uint64_t uid,
                         const cm::protocol::UploadInitRq& rq,
                         std::uint32_t totalChunks,
                         cm::protocol::UploadInitRs* out);

    cm::db::MysqlPool& m_mysql;
    cm::cache::CacheStore& m_cache;
};

}  // namespace logic
}  // namespace cm
