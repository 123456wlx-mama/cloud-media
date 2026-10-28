#include "logic/download_service.h"

#include <unistd.h>

#include <chrono>
#include <memory>
#include <string>

#include "cache/cache_store.h"
#include "cache/redis_pool.h"
#include "cm/log/logger.h"
#include "db/file_dao.h"
#include "db/mysql_conn.h"
#include "db/mysql_pool.h"

namespace cm {
namespace logic {
namespace {

const std::uint32_t kChunkSize = 4194304;  // 与上传分片/下载块一致

bool parseU64Field(const std::string& text, std::uint64_t* value) {
    if (text.empty() || value == nullptr) {
        return false;
    }
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    *value = v;
    return true;
}

// share_ticket value = "ownerUid|fid|code"（ShareService::open 写入）。
bool parseShareTicketValue(const std::string& value, std::uint64_t* ownerUid,
                           std::uint64_t* fid) {
    const std::string::size_type p1 = value.find('|');
    if (p1 == std::string::npos) {
        return false;
    }
    const std::string::size_type p2 = value.find('|', p1 + 1);
    if (p2 == std::string::npos) {
        return false;
    }
    return parseU64Field(value.substr(0, p1), ownerUid) &&
           parseU64Field(value.substr(p1 + 1, p2 - p1 - 1), fid);
}

std::uint8_t zeroCopyFlag() {
#if defined(CM_DOWNLOAD_ZEROCOPY) && (CM_DOWNLOAD_ZEROCOPY)
    return 1;
#else
    return 0;
#endif
}

}  // namespace

DownloadService::DownloadService(cm::db::MysqlPool& mysql,
                                 cm::cache::RedisPool& redis,
                                 const std::string& dataDir)
    : m_mysql(mysql), m_redis(redis), m_ticket(redis), m_dataDir(dataDir) {
}

ErrorCode DownloadService::openDownload(
    std::uint64_t uid,
    const cm::protocol::DownloadOpenRq& rq,
    cm::protocol::DownloadOpenRs* out) {
    if (out == nullptr) {
        return ErrorCode::InvalidArgument;
    }
    *out = cm::protocol::DownloadOpenRs();

    if (uid == 0) {
        out->code = static_cast<std::uint32_t>(ErrorCode::PermissionDenied);
        return ErrorCode::PermissionDenied;
    }
    // 本人节点或分享票据二选一；两者都空是参数错误。
    if (rq.ufId == 0 && rq.shareTicket.empty()) {
        out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        return ErrorCode::InvalidArgument;
    }

    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        const ErrorCode code = (m_mysql.lastError() == ErrorCode::Ok)
                                   ? ErrorCode::DbPoolTimeout
                                   : m_mysql.lastError();
        out->code = static_cast<std::uint32_t>(code);
        return code;
    }

    cm::db::FileDao fileDao(*conn);
    cm::db::BlobRow blob;
    ErrorCode rc = ErrorCode::Ok;

    if (rq.ufId != 0) {
        // 不存在 / 非本人 / 已删除都回 1002，不泄露节点是否存在。
        cm::db::UserFileRow node;
        rc = fileDao.findUserFile(uid, rq.ufId, &node);
        if (rc == ErrorCode::NotFound) {
            out->code = static_cast<std::uint32_t>(ErrorCode::NotFound);
            return ErrorCode::NotFound;
        }
        if (rc != ErrorCode::Ok) {
            out->code = static_cast<std::uint32_t>(cm::to_wire_code(rc));
            return rc;
        }
        if (node.deletedAt != cm::db::FileDao::activeDeletedAt()) {
            out->code = static_cast<std::uint32_t>(ErrorCode::NotFound);
            return ErrorCode::NotFound;
        }
        // 目录节点的 fid 恒为 0，必须先判 isDir，否则永远落到 1002。
        if (node.isDir) {
            out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
            return ErrorCode::InvalidArgument;
        }
        if (node.fid == 0) {
            out->code = static_cast<std::uint32_t>(ErrorCode::NotFound);
            return ErrorCode::NotFound;
        }

        rc = fileDao.getBlobById(node.fid, &blob);
        if (rc == ErrorCode::NotFound) {
            out->code = static_cast<std::uint32_t>(ErrorCode::FileGone);
            return ErrorCode::FileGone;
        }
        if (rc != ErrorCode::Ok) {
            out->code = static_cast<std::uint32_t>(cm::to_wire_code(rc));
            return rc;
        }
    } else {
        // 分享票据：Redis 是唯一授权源，缺失/错误/解析失败一律 fail-closed。
        std::unique_ptr<cm::cache::RedisConn> redis = m_redis.acquire();
        if (!redis) {
            out->code =
                static_cast<std::uint32_t>(ErrorCode::RedisUnavailable);
            return ErrorCode::RedisUnavailable;
        }
        std::string value;
        const cm::cache::RedisLookup lookup =
            redis->get(cm::cache::CacheKey::shareTicket(rq.shareTicket),
                       &value);
        if (lookup == cm::cache::RedisLookup::Error) {
            out->code =
                static_cast<std::uint32_t>(ErrorCode::RedisUnavailable);
            return ErrorCode::RedisUnavailable;
        }
        if (lookup == cm::cache::RedisLookup::Miss) {
            out->code = static_cast<std::uint32_t>(ErrorCode::PermissionDenied);
            return ErrorCode::PermissionDenied;
        }
        std::uint64_t ownerUid = 0;
        std::uint64_t fid = 0;
        if (!parseShareTicketValue(value, &ownerUid, &fid) || fid == 0) {
            out->code = static_cast<std::uint32_t>(ErrorCode::PermissionDenied);
            return ErrorCode::PermissionDenied;
        }

        rc = fileDao.getBlobById(fid, &blob);
        if (rc == ErrorCode::NotFound) {
            out->code = static_cast<std::uint32_t>(ErrorCode::FileGone);
            return ErrorCode::FileGone;
        }
        if (rc != ErrorCode::Ok) {
            out->code = static_cast<std::uint32_t>(cm::to_wire_code(rc));
            return rc;
        }
    }
    if (blob.state != 1 || blob.size == 0) {
        out->code = static_cast<std::uint32_t>(ErrorCode::FileGone);
        return ErrorCode::FileGone;
    }

    // 元数据说"就绪"，物理文件却可能已被磁盘故障/误删带走。这里只做标记、不改
    // 本次请求的结果：open 仍照常发票据，真正读块时由 block_sender 回 3001。
    // 标记之后秒传不再命中它（只有 f_state=1 才可秒传），真实上传能把它改回 1。
    {
        const std::string blobPath = m_dataDir + "/blobs/" + blob.path;
        if (::access(blobPath.c_str(), F_OK) != 0) {
            fileDao.markBlobMissing(blob.fid);
            Logger::instance().log(
                LogLevel::Warn, __FILE__, __LINE__,
                "blob file missing on disk, mark f_state=3 (fid=" +
                    std::to_string(blob.fid) + ", path=" + blob.path + ")");
        }
    }

    const std::uint64_t blockCount =
        (blob.size + kChunkSize - 1) / kChunkSize;
    if (blockCount == 0 || blockCount > 0xFFFFFFFFULL ||
        rq.startBlock >= blockCount) {
        out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        return ErrorCode::InvalidArgument;
    }

    std::string ticket;
    std::uint64_t ticketExpireMs = 0;
    rc = m_ticket.issue(uid, blob.fid, &ticket, &ticketExpireMs);
    if (rc != ErrorCode::Ok) {
        // 票据是授权凭证：Redis 写失败必须拒绝，不 fail-open。
        out->code = static_cast<std::uint32_t>(cm::to_wire_code(rc));
        return rc;
    }

    const std::string hlsPath =
        m_dataDir + "/blobs/" + blob.path + ".m3u8";

    out->code = 0;
    out->fId = blob.fid;
    out->size = blob.size;
    out->fileMd5 = blob.md5;
    out->blockSize = kChunkSize;
    out->blockCount = static_cast<std::uint32_t>(blockCount);
    out->ticket = ticket;
    out->ticketExpireMs = ticketExpireMs;
    out->startBlock = rq.startBlock;
    out->zeroCopy = zeroCopyFlag();
    out->hls = (::access(hlsPath.c_str(), F_OK) == 0) ? 1 : 0;
    conn.reset();
    return ErrorCode::Ok;
}

ErrorCode DownloadService::stat(
    std::uint64_t uid,
    const cm::protocol::DownloadStatRq& rq,
    cm::protocol::DownloadStatRs* out) {
    if (out == nullptr) {
        return ErrorCode::InvalidArgument;
    }
    *out = cm::protocol::DownloadStatRs();

    if (uid == 0) {
        out->code = static_cast<std::uint32_t>(ErrorCode::PermissionDenied);
        return ErrorCode::PermissionDenied;
    }
    if (rq.ufId == 0) {
        out->code = static_cast<std::uint32_t>(ErrorCode::NotFound);
        return ErrorCode::NotFound;
    }

    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        const ErrorCode code = (m_mysql.lastError() == ErrorCode::Ok)
                                   ? ErrorCode::DbPoolTimeout
                                   : m_mysql.lastError();
        out->code = static_cast<std::uint32_t>(code);
        return code;
    }

    cm::db::FileDao dao(*conn);

    cm::db::UserFileRow node;
    ErrorCode rc = dao.findUserFile(uid, rq.ufId, &node);
    if (rc == ErrorCode::NotFound) {
        out->code = static_cast<std::uint32_t>(ErrorCode::NotFound);
        return ErrorCode::NotFound;
    }
    if (rc != ErrorCode::Ok) {
        out->code = static_cast<std::uint32_t>(cm::to_wire_code(rc));
        return rc;
    }
    if (node.deletedAt != cm::db::FileDao::activeDeletedAt()) {
        out->code = static_cast<std::uint32_t>(ErrorCode::NotFound);
        return ErrorCode::NotFound;
    }
    if (node.isDir) {
        out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        return ErrorCode::InvalidArgument;
    }
    if (node.fid == 0) {
        out->code = static_cast<std::uint32_t>(ErrorCode::NotFound);
        return ErrorCode::NotFound;
    }

    cm::db::BlobRow blob;
    rc = dao.getBlobById(node.fid, &blob);
    if (rc == ErrorCode::NotFound) {
        out->code = static_cast<std::uint32_t>(ErrorCode::FileGone);
        return ErrorCode::FileGone;
    }
    if (rc != ErrorCode::Ok) {
        out->code = static_cast<std::uint32_t>(cm::to_wire_code(rc));
        return rc;
    }
    if (blob.state != 1 || blob.size == 0) {
        out->code = static_cast<std::uint32_t>(ErrorCode::FileGone);
        return ErrorCode::FileGone;
    }

    const std::uint64_t blockCount =
        (blob.size + kChunkSize - 1) / kChunkSize;
    if (blockCount == 0 || blockCount > 0xFFFFFFFFULL) {
        out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        return ErrorCode::InvalidArgument;
    }

    out->code = 0;
    out->fId = blob.fid;
    out->size = blob.size;
    out->fileMd5 = blob.md5;
    out->blockSize = kChunkSize;
    out->blockCount = static_cast<std::uint32_t>(blockCount);
    out->state = blob.state;
    out->updatedAtMs = 0;  // BlobRow 未含 updated_at，本期不返回
    conn.reset();
    return ErrorCode::Ok;
}

ErrorCode DownloadService::done(
    std::uint64_t uid,
    const cm::protocol::DownloadDoneRq& rq,
    cm::protocol::DownloadDoneRs* out) {
    if (out == nullptr) {
        return ErrorCode::InvalidArgument;
    }
    *out = cm::protocol::DownloadDoneRs();

    if (uid == 0) {
        out->code = static_cast<std::uint32_t>(ErrorCode::PermissionDenied);
        return ErrorCode::PermissionDenied;
    }

    // 1. 校验 ticket（verify 不续期）。uid/fid 不符/过期 → 1004。
    DownloadTicketInfo info;
    ErrorCode rc = m_ticket.verify(rq.ticket, uid, rq.fId, &info);
    if (rc != ErrorCode::Ok) {
        out->code = static_cast<std::uint32_t>(cm::to_wire_code(rc));
        return rc;
    }

    // 2. Redis 去重 10min：SET cm:v1:dl:done:{uid}:{ticket} NX PX 600000。
    //    已存在 → 幂等返回；Redis 不可用 → 跳过去重（06 §10.1，重复计数可接受）。
    bool alreadyReported = false;
    std::unique_ptr<cm::cache::RedisConn> redisConn = m_redis.acquire();
    if (redisConn) {
        const std::string key =
            cm::cache::CacheKey::downloadDone(uid, rq.ticket);
        if (!key.empty()) {
            if (redisConn->setNxPx(key, "1",
                                   std::chrono::milliseconds(600000))) {
                // 首次上报，继续。
            } else if (redisConn->lastError() == ErrorCode::Ok) {
                alreadyReported = true;
            }
        }
    }

    if (alreadyReported) {
        out->code = 0;
        out->accepted = 1;
        return ErrorCode::Ok;
    }

    // 3. 校验结果：verify_ok=0 仅告警，不阻断（下载正确性由客户端整文件 MD5 保证）。
    if (rq.verifyOk == 0) {
        // 客户端自报 MD5 与 open 返回的不符：说明传输过程中数据被改坏，
        // 不阻断（客户端会重下），但必须留痕才能定位是网络还是磁盘问题。
        Logger::instance().log(
            LogLevel::Warn, __FILE__, __LINE__,
            "download verify_ok=0 (uid=" + std::to_string(uid) +
                ", fid=" + std::to_string(rq.fId) +
                ", size=" + std::to_string(rq.size) + ")");
        out->code = static_cast<std::uint32_t>(ErrorCode::Md5Mismatch);
        out->accepted = 0;
        return ErrorCode::Md5Mismatch;
    }

    out->code = 0;
    out->accepted = 1;
    return ErrorCode::Ok;
}

}  // namespace logic
}  // namespace cm
