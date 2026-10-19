#include "file_service.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "db/file_dao.h"
#include "db/mysql_conn.h"
#include "db/mysql_pool.h"

namespace cm {
namespace logic {
namespace {

ErrorCode dbPoolError(cm::db::MysqlPool& pool) {
    return pool.lastError() == ErrorCode::Ok ? ErrorCode::DbPoolTimeout
                                             : pool.lastError();
}

void setInvalid(cm::protocol::ListFileRs* out) {
    out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
}

// DATETIME(6) 文本 "YYYY-MM-DD HH:MM:SS.ffffff"，长度固定 26。
bool isDatetime6(const std::string& text) {
    if (text.size() != 26) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (i == 4 || i == 7) {
            if (c != '-') {
                return false;
            }
        } else if (i == 10) {
            if (c != ' ') {
                return false;
            }
        } else if (i == 13 || i == 16) {
            if (c != ':') {
                return false;
            }
        } else if (i == 19) {
            if (c != '.') {
                return false;
            }
        } else if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

// 1970-01-01 起的天数（civil 日期算法，避免依赖 timegm）。
std::int64_t daysFromCivil(int year, unsigned month, unsigned day) {
    year -= (month <= 2);
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 +
           static_cast<std::int64_t>(doe) - 719468;
}

// MySQL DATETIME(6)（连接池固定 UTC）→ epoch 毫秒。解析失败返回 0。
std::uint64_t datetimeToMs(const std::string& text) {
    if (text.size() < 19) {
        return 0;
    }
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day,
                    &hour, &minute, &second) != 6) {
        return 0;
    }
    if (year < 1970) {
        return 0;
    }
    std::uint64_t ms = 0;
    if (text.size() > 19 && text[19] == '.') {
        std::uint64_t micros = 0;
        for (std::size_t i = 20; i < text.size() && i < 26; ++i) {
            const char c = text[i];
            if (c < '0' || c > '9') {
                break;
            }
            micros = micros * 10 + static_cast<std::uint64_t>(c - '0');
        }
        ms = micros / 1000;
    }
    const std::int64_t days = daysFromCivil(
        year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    return static_cast<std::uint64_t>(days) * 86400000ULL +
           static_cast<std::uint64_t>(hour) * 3600000ULL +
           static_cast<std::uint64_t>(minute) * 60000ULL +
           static_cast<std::uint64_t>(second) * 1000ULL + ms;
}

}  // namespace

const std::uint64_t FileService::kUnknownTotalCount;
const std::size_t FileService::kDefaultLimit;
const std::size_t FileService::kMaxLimit;
const std::uint64_t FileService::kDefaultMaxTotalCount;

FileService::FileService(cm::db::MysqlPool& mysql, std::uint64_t maxTotalCount)
    : m_mysql(mysql), m_maxTotalCount(maxTotalCount) {
}

ErrorCode FileService::list(std::uint64_t uid,
                            const cm::protocol::ListFileRq& rq,
                            cm::protocol::ListFileRs* out) {
    if (out == NULL) {
        return ErrorCode::InvalidArgument;
    }
    *out = cm::protocol::ListFileRs();

    if (uid == 0) {
        out->code = static_cast<std::uint32_t>(ErrorCode::PermissionDenied);
        return ErrorCode::PermissionDenied;
    }
    if (rq.sort > 2) {
        setInvalid(out);
        return ErrorCode::InvalidArgument;
    }

    std::size_t limit = rq.limit == 0 ? kDefaultLimit : rq.limit;
    if (limit > kMaxLimit) {
        setInvalid(out);
        return ErrorCode::InvalidArgument;
    }

    // 首页 = cursorKey 为空且 cursorId 为 0；半空游标一律拒绝。
    const bool firstPage = rq.cursorKey.empty() && rq.cursorId == 0;
    if (!firstPage) {
        if (rq.cursorKey.empty() || rq.cursorId == 0) {
            setInvalid(out);
            return ErrorCode::InvalidArgument;
        }
        if (rq.sort == 0) {
            // 名称游标来自 uf_name，最长 255 字节。
            if (rq.cursorKey.size() > 255) {
                setInvalid(out);
                return ErrorCode::InvalidArgument;
            }
        } else if (!isDatetime6(rq.cursorKey)) {
            // 时间游标必须是上一页末行的 DATETIME(6) 文本。
            setInvalid(out);
            return ErrorCode::InvalidArgument;
        }
    }

    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        const ErrorCode code = dbPoolError(m_mysql);
        out->code = static_cast<std::uint32_t>(code);
        return code;
    }

    cm::db::FileListQuery query;
    query.uid = uid;
    query.parentId = rq.parentId;
    query.sort = static_cast<cm::db::FileListSort>(rq.sort);
    query.limit = limit;
    query.cursorKey = rq.cursorKey;
    query.cursorId = rq.cursorId;

    cm::db::FileDao fileDao(*conn);
    cm::db::FileListPage page;
    const ErrorCode rc = fileDao.listByParent(query, &page);
    if (rc != ErrorCode::Ok) {
        conn.reset();
        out->code = static_cast<std::uint32_t>(rc);
        return rc;
    }

    std::uint64_t total = kUnknownTotalCount;
    if (firstPage) {
        std::uint64_t counted = 0;
        const ErrorCode countRc =
            fileDao.countByParent(uid, rq.parentId, &counted);
        // COUNT 失败/超时只降级为“未知”，不影响本页列表。
        if (countRc == ErrorCode::Ok &&
            counted != cm::db::FileDao::kUnknownCount) {
            total = counted > m_maxTotalCount ? m_maxTotalCount : counted;
        }
    }
    conn.reset();

    out->code = 0;
    out->items.reserve(page.rows.size());
    for (std::size_t i = 0; i < page.rows.size(); ++i) {
        const cm::db::UserFileRow& row = page.rows[i];
        cm::protocol::FileItem item;
        item.ufId = row.ufId;
        item.fId = row.fid;
        item.name = row.name;
        item.isDir = row.isDir ? 1 : 0;
        item.size = row.isDir ? 0 : row.size;
        item.md5 = row.isDir ? std::string() : row.md5;
        item.createdAtMs = datetimeToMs(row.createdAt);
        item.updatedAtMs = datetimeToMs(row.updatedAt);
        out->items.push_back(item);
    }
    out->hasMore = page.hasMore ? 1 : 0;
    out->nextKey = page.nextKey;
    out->nextId = page.nextId;
    out->totalCount = total;
    return ErrorCode::Ok;
}

}  // namespace logic
}  // namespace cm
