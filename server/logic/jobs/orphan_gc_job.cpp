#include "logic/jobs/orphan_gc_job.h"

#include <memory>
#include <vector>

#include "db/file_dao.h"
#include "db/mysql_conn.h"
#include "db/mysql_pool.h"
#include "db/prepared_statement.h"

namespace cm {
namespace logic {
namespace {

ErrorCode dbPoolError(cm::db::MysqlPool& pool) {
    return pool.lastError() == ErrorCode::Ok ? ErrorCode::DbPoolTimeout
                                             : pool.lastError();
}

}  // namespace

OrphanGcJob::OrphanGcJob(cm::db::MysqlPool& mysql,
                         const std::string& dataDir,
                         BlobUnlinker unlinker,
                         std::uint64_t graceSeconds,
                         std::size_t maxPerRound,
                         std::uint64_t maxBytesPerRound)
    : m_mysql(mysql),
      m_dataDir(dataDir),
      m_unlinker(unlinker),
      m_graceSeconds(graceSeconds),
      m_maxPerRound(maxPerRound),
      m_maxBytesPerRound(maxBytesPerRound) {
}

ErrorCode OrphanGcJob::runOnce(std::size_t* deleted,
                               std::uint64_t* freedBytes) {
    if (deleted != nullptr) {
        *deleted = 0;
    }
    if (freedBytes != nullptr) {
        *freedBytes = 0;
    }

    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        return dbPoolError(m_mysql);
    }

    cm::db::FileDao fileDao(*conn);
    std::vector<cm::db::OrphanBlobRow> rows;
    const ErrorCode rc =
        fileDao.listOrphanBlobs(m_graceSeconds, m_maxPerRound, &rows);
    if (rc != ErrorCode::Ok) {
        return rc;
    }

    std::size_t purged = 0;
    std::uint64_t bytes = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        // 单轮 2GB：至少处理一条，避免超大头文件永远清不掉。
        if (purged > 0 && bytes + rows[i].size > m_maxBytesPerRound) {
            break;
        }
        if (purgeOne(*conn, rows[i])) {
            ++purged;
            bytes += rows[i].size;
        }
    }

    if (deleted != nullptr) {
        *deleted = purged;
    }
    if (freedBytes != nullptr) {
        *freedBytes = bytes;
    }
    return ErrorCode::Ok;
}

bool OrphanGcJob::purgeOne(cm::db::MysqlConn& conn,
                           const cm::db::OrphanBlobRow& row) {
    // 复用扫描用的连接：listOrphanBlobs 已取尽结果集，可以安全执行事务。
    cm::db::Transaction tx(conn);
    if (!tx.valid()) {
        return false;
    }

    // 第二道复核：锁行并重新确认（状态 / 引用 / 节点 / 分享）。
    // 节点条件必须与候选查询一致：只剩回收站节点时不允许回收，
    // 必须等用户彻底删除（purge）后才真正释放物理空间。
    std::unique_ptr<cm::db::PreparedStatement> locked = conn.prepare(
        "SELECT 1 FROM t_file f "
        "WHERE f.f_id = ? AND f.f_state = 2 AND f.f_ref_count = 0 "
        "AND NOT EXISTS (SELECT 1 FROM t_user_file uf WHERE uf.f_id = f.f_id) "
        "AND NOT EXISTS (SELECT 1 FROM t_share s "
        "WHERE s.s_file_id = f.f_id AND s.s_status = 0 "
        "AND s.s_expire_at >= NOW(6)) "
        "FOR UPDATE");
    if (!locked) {
        return false;
    }
    if (!locked->bindUInt64(0, row.fid) || !locked->execute()) {
        return false;
    }
    const cm::db::PreparedStatement::FetchStatus status = locked->fetchRow();
    if (status != cm::db::PreparedStatement::FetchStatus::Row) {
        return false;  // 期间被重新引用/状态变化
    }
    // 释放结果集再在同一连接上执行 DELETE，避免 commands out of sync。
    locked.reset();

    const std::string blobPath = m_dataDir + "/blobs/" + row.path;
    if (!m_unlinker || !m_unlinker(blobPath)) {
        return false;  // 回滚，保留行等下一轮
    }
    if (!m_unlinker(blobPath + ".crc")) {
        return false;  // sidecar 失败同样保留行（blob 已删，下轮 ENOENT 通过）
    }
    // .meta 也要删：上传会写 <blob>.meta，只删 blob/.crc 会让每个被回收的 blob
    // 留下一个永久孤儿文件。ENOENT 由 unlinker 视为成功，不影响幂等。
    if (!m_unlinker(blobPath + ".meta")) {
        return false;
    }

    std::unique_ptr<cm::db::PreparedStatement> remove =
        conn.prepare("DELETE FROM t_file WHERE f_id = ?");
    if (!remove || !remove->bindUInt64(0, row.fid) || !remove->execute()) {
        return false;
    }
    if (remove->affectedRows() != 1) {
        return false;
    }
    return tx.commit();
}

}  // namespace logic
}  // namespace cm
