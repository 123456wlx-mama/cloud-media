#include "logic/jobs/share_expiry_job.h"

#include <memory>

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

ShareExpiryJob::ShareExpiryJob(cm::db::MysqlPool& mysql, std::size_t batch)
    : m_mysql(mysql), m_batch(batch) {
}

ErrorCode ShareExpiryJob::runOnce(std::uint64_t* affected) {
    if (affected != nullptr) {
        *affected = 0;
    }
    if (m_batch == 0) {
        return ErrorCode::InvalidArgument;
    }

    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        return dbPoolError(m_mysql);
    }

    cm::db::ShareDao dao(*conn);
    return dao.sweepExpired(m_batch, affected);
}

}  // namespace logic
}  // namespace cm
