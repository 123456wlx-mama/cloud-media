#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "cm/base/error_code.h"
#include "cm/protocol/messages/recycle.h"

namespace cm {

namespace db {
class MysqlPool;
}  // namespace db

namespace cache {
class CacheStore;
}  // namespace cache

namespace logic {

// 回收站服务：10025 软删、10034 回收站列表、10036 恢复、10038 彻底删除。
// 只经 FileDao/ShareDao 与 CacheStore 访问存储，不出现 MySQL/Redis C API。
class RecycleService {
public:
    static const std::size_t kDefaultMaxPurgeNodes = 10000;  // 06 §8.2
    static const std::size_t kPurgeBatchSize = 200;          // 200 行/事务

    // maxPurgeNodes 可注入（测试 TooLarge 用较小值，避免造 10001 行）。
    RecycleService(cm::db::MysqlPool& mysql,
                   cm::cache::CacheStore& cache,
                   std::size_t maxPurgeNodes = kDefaultMaxPurgeNodes);

    RecycleService(const RecycleService&) = delete;
    RecycleService& operator=(const RecycleService&) = delete;

    // 10025：软删（事务内锁行 → 已删短路 → 置 deleted_at → 文件减计数 → 0 置孤儿），
    // 成功后失效该节点的分享缓存。
    cm::ErrorCode deleteFile(std::uint64_t uid,
                             const cm::protocol::DeleteFileRq& rq,
                             cm::protocol::DeleteFileRs* out);

    // 10034：回收站分页（keyset 游标 (deleted_ms, uf_id)，禁止 OFFSET）。
    cm::ErrorCode listRecycle(std::uint64_t uid,
                              const cm::protocol::RecycleListRq& rq,
                              cm::protocol::RecycleListRs* out);

    // 10036：恢复（父缺失/已删回根；名称冲突自动改名；已恢复幂等返回）。
    cm::ErrorCode restore(std::uint64_t uid,
                          const cm::protocol::RestoreRq& rq,
                          cm::protocol::RestoreRs* out);

    // 10038：彻底删除。文件只删行；目录 BFS 子树（>maxPurgeNodes 回 3008 不部分删除），
    // 200 行/事务、子先父后根最后删、可重跑；活跃文件补减计数、已软删不二次减；每批取消分享。
    cm::ErrorCode purge(std::uint64_t uid,
                        const cm::protocol::PurgeRq& rq,
                        cm::protocol::PurgeRs* out);

private:
    cm::db::MysqlPool& m_mysql;
    cm::cache::CacheStore& m_cache;
    std::size_t m_maxPurgeNodes;
};

}  // namespace logic
}  // namespace cm
