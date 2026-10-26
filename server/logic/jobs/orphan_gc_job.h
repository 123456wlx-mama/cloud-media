#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "cm/base/error_code.h"

namespace cm {
namespace db {
class MysqlConn;
class MysqlPool;
struct OrphanBlobRow;
}  // namespace db

namespace logic {

// 孤儿 blob 日清理。
// 候选：f_state=2 + 超过宽限期 + f_ref_count=0 + 不存在任何用户节点 + 无有效分享。
// 注意「不存在任何节点」而不是「无活跃节点」：回收站里的节点（uf_deleted_at
// 非哨兵）依然要能恢复，物理回收必须在用户彻底删除之后才发生。
// 单轮上限由构造参数决定（生产 100 条 / 2GB）。
//
// 并发安全：候选只是“第一道筛选”。真正删除前在事务内 SELECT ... FOR UPDATE
// 复核同样条件，持锁删除文件与行，避免期间被秒传重新引用或被恢复。
// unlink 由业务池线程执行（本 job 只从 ServerApp 的业务池调用，绝不落到 loop）；
// 失败时回滚，行保留到下一轮重试。
class OrphanGcJob {
public:
    // 返回 true 表示文件已不存在（含 ENOENT）；false 表示失败、保留行。
    using BlobUnlinker = std::function<bool(const std::string& path)>;

    OrphanGcJob(cm::db::MysqlPool& mysql,
                const std::string& dataDir,
                BlobUnlinker unlinker,
                std::uint64_t graceSeconds = 24ULL * 3600ULL,
                std::size_t maxPerRound = 100,
                std::uint64_t maxBytesPerRound = 2ULL * 1024ULL * 1024ULL * 1024ULL);

    OrphanGcJob(const OrphanGcJob&) = delete;
    OrphanGcJob& operator=(const OrphanGcJob&) = delete;

    // 返回本轮成功删除的 blob 数与释放字节数（均可为 NULL）。
    cm::ErrorCode runOnce(std::size_t* deleted, std::uint64_t* freedBytes);

private:
    // 事务内复核 + unlink + 删行；true 表示本行已删除。
    bool purgeOne(cm::db::MysqlConn& conn, const cm::db::OrphanBlobRow& row);

    cm::db::MysqlPool& m_mysql;
    std::string m_dataDir;
    BlobUnlinker m_unlinker;
    std::uint64_t m_graceSeconds;
    std::size_t m_maxPerRound;
    std::uint64_t m_maxBytesPerRound;
};

}  // namespace logic
}  // namespace cm
