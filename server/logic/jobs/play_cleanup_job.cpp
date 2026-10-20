#include "logic/jobs/play_cleanup_job.h"

#include <memory>

#include "db/mysql_conn.h"
#include "db/mysql_pool.h"
#include "db/play_dao.h"

namespace cm {
namespace logic {
namespace {

ErrorCode dbPoolError(cm::db::MysqlPool& pool) {
    return pool.lastError() == ErrorCode::Ok ? ErrorCode::DbPoolTimeout
                                             : pool.lastError();
}

}  // namespace

PlayCleanupJob::PlayCleanupJob(cm::db::MysqlPool& mysql, std::size_t batch)
    : m_mysql(mysql), m_batch(batch) {
}

ErrorCode PlayCleanupJob::runOnce(std::uint64_t* deleted) {
    if (deleted != nullptr) {
        *deleted = 0;
    }
    if (m_batch == 0) {
        return ErrorCode::InvalidArgument;
    }

    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        return dbPoolError(m_mysql);
    }

    cm::db::PlayDao dao(*conn);
    return dao.deleteOrphanRecords(m_batch, deleted);
}

}  // namespace logic
}  // namespace cm
