#include "recycle_service.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cache/cache_store.h"
#include "db/file_dao.h"
#include "db/mysql_conn.h"
#include "db/mysql_pool.h"
#include "db/share_dao.h"

namespace cm {
namespace logic {
namespace {

ErrorCode dbPoolError(cm::db::MysqlPool& pool) {
    return pool.lastError() == ErrorCode::Ok ? ErrorCode::DbPoolTimeout
                                             : pool.lastError();
}

}  // namespace

// C++11：类内初始化仅给值；被 odr-use（std::min 以 const 引用绑定）时仍需类外定义。
const std::size_t RecycleService::kDefaultMaxPurgeNodes;
const std::size_t RecycleService::kPurgeBatchSize;

RecycleService::RecycleService(cm::db::MysqlPool& mysql,
                               cm::cache::CacheStore& cache,
                               std::size_t maxPurgeNodes)
    : m_mysql(mysql), m_cache(cache), m_maxPurgeNodes(maxPurgeNodes) {
}

ErrorCode RecycleService::deleteFile(std::uint64_t uid,
                                     const cm::protocol::DeleteFileRq& rq,
                                     cm::protocol::DeleteFileRs* out) {
    if (out == NULL) {
        return ErrorCode::InvalidArgument;
    }
    *out = cm::protocol::DeleteFileRs();

    if (uid == 0 || rq.ufId == 0) {
        out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        return ErrorCode::InvalidArgument;
    }

    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        const ErrorCode code = dbPoolError(m_mysql);
        out->code = static_cast<std::uint32_t>(code);
        return code;
    }

    cm::db::FileDao fileDao(*conn);
    bool already = false;
    const ErrorCode rc = fileDao.softDelete(uid, rq.ufId, &already);
    if (rc != ErrorCode::Ok) {
        conn.reset();
        out->code = static_cast<std::uint32_t>(rc);
        return rc;
    }

    // 失效指向该节点的分享缓存（best-effort；访问侧还会校验节点已删）。
    cm::db::ShareDao shareDao(*conn);
    std::vector<std::string> codes;
    const ErrorCode shareRc = shareDao.listActiveByUfId(rq.ufId, &codes);
    conn.reset();
    if (shareRc == ErrorCode::Ok) {
        for (std::size_t i = 0; i < codes.size(); ++i) {
            m_cache.invalidate(cm::cache::CacheKey::share(codes[i]));
        }
    }

    out->code = 0;
    out->deleted = already ? 0 : 1;
    out->already = already ? 1 : 0;
    return ErrorCode::Ok;
}

ErrorCode RecycleService::listRecycle(std::uint64_t uid,
                                      const cm::protocol::RecycleListRq& rq,
                                      cm::protocol::RecycleListRs* out) {
    if (out == NULL) {
        return ErrorCode::InvalidArgument;
    }
    *out = cm::protocol::RecycleListRs();

    if (uid == 0 || rq.limit == 0 || rq.limit > 200) {
        out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        return ErrorCode::InvalidArgument;
    }
    const bool firstPage = rq.cursorDeletedMs == 0 && rq.cursorId == 0;
    if (!firstPage && (rq.cursorDeletedMs == 0 || rq.cursorId == 0)) {
        out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        return ErrorCode::InvalidArgument;
    }

    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        const ErrorCode code = dbPoolError(m_mysql);
        out->code = static_cast<std::uint32_t>(code);
        return code;
    }
    cm::db::FileDao fileDao(*conn);
    cm::db::RecycleListQuery query;
    query.uid = uid;
    query.limit = rq.limit;
    query.cursorDeletedMs = rq.cursorDeletedMs;
    query.cursorId = rq.cursorId;
    cm::db::RecycleListPage page;
    const ErrorCode rc = fileDao.listRecycle(query, &page);
    conn.reset();
    if (rc != ErrorCode::Ok) {
        out->code = static_cast<std::uint32_t>(rc);
        return rc;
    }

    out->code = 0;
    out->items.reserve(page.rows.size());
    for (std::size_t i = 0; i < page.rows.size(); ++i) {
        const cm::db::RecycleNode& node = page.rows[i];
        cm::protocol::RecycleItem item;
        item.ufId = node.ufId;
        item.name = node.name;
        item.isDir = node.isDir ? 1 : 0;
        item.size = node.size;
        item.deletedAtMs = node.deletedAtMs;
        item.parentId = node.parentId;
        out->items.push_back(item);
    }
    out->hasMore = page.hasMore ? 1 : 0;
    out->nextDeletedMs = page.nextDeletedMs;
    out->nextId = page.nextId;
    return ErrorCode::Ok;
}

ErrorCode RecycleService::restore(std::uint64_t uid,
                                  const cm::protocol::RestoreRq& rq,
                                  cm::protocol::RestoreRs* out) {
    if (out == NULL) {
        return ErrorCode::InvalidArgument;
    }
    *out = cm::protocol::RestoreRs();

    if (uid == 0 || rq.ufId == 0 || rq.autoRename > 1 || rq.newName.size() > 255) {
        out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        return ErrorCode::InvalidArgument;
    }

    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        const ErrorCode code = dbPoolError(m_mysql);
        out->code = static_cast<std::uint32_t>(code);
        return code;
    }

    cm::db::FileDao fileDao(*conn);
    cm::db::UserFileRow node;
    ErrorCode rc = fileDao.findUserFile(uid, rq.ufId, &node);
    if (rc != ErrorCode::Ok) {
        conn.reset();
        out->code = static_cast<std::uint32_t>(rc);
        return rc;
    }

    // 父目录缺失/已删/非目录 → 回根(0)。
    std::uint64_t parentId = node.parentId;
    if (parentId != 0) {
        cm::db::UserFileRow parent;
        rc = fileDao.findUserFile(uid, parentId, &parent);
        if (rc == ErrorCode::NotFound ||
            (rc == ErrorCode::Ok &&
             (parent.deletedAt != cm::db::FileDao::activeDeletedAt() ||
              !parent.isDir))) {
            parentId = 0;
        } else if (rc != ErrorCode::Ok) {
            conn.reset();
            out->code = static_cast<std::uint32_t>(rc);
            return rc;
        }
    }

    const std::string requested = rq.newName.empty() ? node.name : rq.newName;
    cm::db::RestoreOutcome outcome;
    rc = fileDao.restore(uid, rq.ufId, parentId, requested, &outcome);
    if (rc != ErrorCode::Ok) {
        conn.reset();
        out->code = static_cast<std::uint32_t>(rc);
        return rc;
    }

    // 读回最终 parent/name（已恢复幂等路径也拿到当前值）。
    cm::db::UserFileRow after;
    rc = fileDao.findUserFile(uid, rq.ufId, &after);
    conn.reset();
    if (rc != ErrorCode::Ok) {
        out->code = static_cast<std::uint32_t>(rc);
        return rc;
    }

    const bool renamed = outcome.alreadyRestored ? false : (after.name != requested);
    out->code = 0;
    out->ufId = rq.ufId;
    out->parentId = after.parentId;
    out->name = after.name;
    out->renamed = renamed ? 1 : 0;
    return ErrorCode::Ok;
}

ErrorCode RecycleService::purge(std::uint64_t uid,
                                const cm::protocol::PurgeRq& rq,
                                cm::protocol::PurgeRs* out) {
    if (out == NULL) {
        return ErrorCode::InvalidArgument;
    }
    *out = cm::protocol::PurgeRs();

    if (uid == 0 || rq.ufId == 0 || rq.confirm != 1) {
        out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        return ErrorCode::InvalidArgument;
    }

    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        const ErrorCode code = dbPoolError(m_mysql);
        out->code = static_cast<std::uint32_t>(code);
        return code;
    }

    cm::db::FileDao fileDao(*conn);
    cm::db::UserFileRow node;
    ErrorCode rc = fileDao.findUserFile(uid, rq.ufId, &node);
    if (rc == ErrorCode::NotFound) {
        // 幂等：节点已不存在 → code=0，三个计数为 0（06 §4.18）。
        conn.reset();
        out->code = 0;
        out->filesDeleted = 0;
        out->dirsDeleted = 0;
        out->blobsOrphaned = 0;
        return ErrorCode::Ok;
    }
    if (rc != ErrorCode::Ok) {
        conn.reset();
        out->code = static_cast<std::uint32_t>(rc);
        return rc;
    }

    cm::db::ShareDao shareDao(*conn);
    std::uint32_t filesDeleted = 0;
    std::uint32_t dirsDeleted = 0;
    std::uint32_t blobsOrphaned = 0;

    if (!node.isDir) {
        // 单文件：一次事务删行 + 补减计数 + 取消分享。
        cm::db::PurgeBatchResult batch;
        ErrorCode branchRc = ErrorCode::Ok;
        {
            cm::db::Transaction tx(*conn);
            if (!tx.valid()) {
                branchRc = (conn->lastError() == ErrorCode::Ok)
                               ? ErrorCode::DbUnavailable
                               : conn->lastError();
            } else {
                std::vector<std::uint64_t> one(1, rq.ufId);
                branchRc = fileDao.purgeNodesInTx(uid, one, &batch);
                if (branchRc == ErrorCode::Ok) {
                    std::uint64_t affected = 0;
                    shareDao.cancelActiveByUfIdInTx(rq.ufId, &affected);
                }
                if (branchRc == ErrorCode::Ok && !tx.commit()) {
                    branchRc = (tx.lastError() == ErrorCode::Ok)
                                   ? ErrorCode::DbQueryFailed
                                   : tx.lastError();
                }
            }
        }  // tx 析构（未 commit 则回滚），此时 conn 仍有效。
        conn.reset();
        if (branchRc != ErrorCode::Ok) {
            out->code = static_cast<std::uint32_t>(branchRc);
            return branchRc;
        }
        filesDeleted = batch.filesDeleted;
        dirsDeleted = batch.dirsDeleted;
        blobsOrphaned = batch.blobsOrphaned;
    } else {
        // 目录：BFS 收集子树 → 判上限 → 反序分批删除（子先父后、根最后）。
        std::vector<cm::db::UserFileRow> subtree;
        rc = fileDao.listSubtree(uid, rq.ufId, m_maxPurgeNodes, &subtree);
        if (rc == ErrorCode::TooManyNodes) {
            conn.reset();
            out->code = static_cast<std::uint32_t>(ErrorCode::TooManyNodes);
            return ErrorCode::TooManyNodes;
        }
        if (rc != ErrorCode::Ok) {
            conn.reset();
            out->code = static_cast<std::uint32_t>(rc);
            return rc;
        }

        std::size_t remaining = subtree.size();
        while (remaining > 0) {
            const std::size_t batchCount =
                std::min(kPurgeBatchSize, remaining);
            std::vector<std::uint64_t> batchIds;
            batchIds.reserve(batchCount);
            for (std::size_t j = remaining - batchCount; j < remaining; ++j) {
                batchIds.push_back(subtree[j].ufId);
            }
            remaining -= batchCount;

            cm::db::PurgeBatchResult batch;
            ErrorCode branchRc = ErrorCode::Ok;
            {
                cm::db::Transaction tx(*conn);
                if (!tx.valid()) {
                    branchRc = (conn->lastError() == ErrorCode::Ok)
                                   ? ErrorCode::DbUnavailable
                                   : conn->lastError();
                } else {
                    branchRc = fileDao.purgeNodesInTx(uid, batchIds, &batch);
                    if (branchRc == ErrorCode::Ok) {
                        for (std::size_t j = 0; j < batchIds.size(); ++j) {
                            std::uint64_t affected = 0;
                            shareDao.cancelActiveByUfIdInTx(batchIds[j],
                                                            &affected);
                        }
                    }
                    if (branchRc == ErrorCode::Ok && !tx.commit()) {
                        branchRc = (tx.lastError() == ErrorCode::Ok)
                                       ? ErrorCode::DbQueryFailed
                                       : tx.lastError();
                    }
                }
            }  // tx 析构（未 commit 则回滚），此时 conn 仍有效。
            if (branchRc != ErrorCode::Ok) {
                conn.reset();
                out->code = static_cast<std::uint32_t>(branchRc);
                return branchRc;
            }
            filesDeleted += batch.filesDeleted;
            dirsDeleted += batch.dirsDeleted;
            blobsOrphaned += batch.blobsOrphaned;
        }
    }

    conn.reset();
    out->code = 0;
    out->filesDeleted = filesDeleted;
    out->dirsDeleted = dirsDeleted;
    out->blobsOrphaned = blobsOrphaned;
    return ErrorCode::Ok;
}

}  // namespace logic
}  // namespace cm
