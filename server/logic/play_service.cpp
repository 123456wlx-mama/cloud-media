#include "play_service.h"

#include <memory>

#include "db/file_dao.h"
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

PlayService::PlayService(cm::db::MysqlPool& mysql) : m_mysql(mysql) {
}

ErrorCode PlayService::report(std::uint64_t uid,
                              const cm::protocol::PlayReportRq& rq,
                              cm::protocol::PlayReportRs* out) {
    if (out == nullptr) {
        return ErrorCode::InvalidArgument;
    }
    *out = cm::protocol::PlayReportRs();

    if (uid == 0) {
        out->code = static_cast<std::uint32_t>(ErrorCode::PermissionDenied);
        return ErrorCode::PermissionDenied;
    }
    // 目录没有 blob，f_id==0 视为文件不存在。
    if (rq.fId == 0) {
        out->code = static_cast<std::uint32_t>(ErrorCode::NotFound);
        return ErrorCode::NotFound;
    }
    if (rq.durationMs > 0 && rq.positionMs > rq.durationMs) {
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
    bool visible = false;
    const ErrorCode visibleRc = fileDao.hasVisibleFileByFid(uid, rq.fId, &visible);
    if (visibleRc != ErrorCode::Ok) {
        conn.reset();
        out->code = static_cast<std::uint32_t>(visibleRc);
        return visibleRc;
    }
    // 不存在与不属于本用户同样回 1002，不泄露他人文件存在性。
    if (!visible) {
        conn.reset();
        out->code = static_cast<std::uint32_t>(ErrorCode::NotFound);
        return ErrorCode::NotFound;
    }

    cm::db::PlayDao playDao(*conn);
    const ErrorCode rc =
        playDao.upsertLww(uid, rq.fId, rq.positionMs, rq.durationMs,
                          rq.finished != 0);
    conn.reset();
    if (rc != ErrorCode::Ok) {
        out->code = static_cast<std::uint32_t>(rc);
        return rc;
    }

    out->code = 0;
    out->saved = 1;
    return ErrorCode::Ok;
}

}  // namespace logic
}  // namespace cm
