#include "logic/download_service.h"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <random>
#include <string>

#include "cache/cache_store.h"
#include "db/file_dao.h"
#include "db/mysql_conn.h"
#include "db/mysql_pool.h"

namespace cm {
namespace logic {
namespace {

const std::uint32_t kChunkSize = 4194304;  // 与上传分片/下载块一致
const std::chrono::milliseconds kTicketTtl(30 * 60 * 1000);

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// 32 字节随机 → 64 位小写 hex。每次调用重新播种，避免业务池并发共享生成器。
std::string makeTicket() {
    std::random_device device;
    std::uint64_t seed = (static_cast<std::uint64_t>(device()) << 32) ^
                         static_cast<std::uint64_t>(device());
    seed ^= nowMs() * 0x9e3779b97f4a7c15ULL;
    seed ^= static_cast<std::uint64_t>(::getpid()) << 24;

    std::mt19937_64 generator(seed);
    char buffer[65];
    for (int i = 0; i < 4; ++i) {
        std::snprintf(buffer + i * 16, 17, "%016llx",
                      static_cast<unsigned long long>(generator()));
    }
    return std::string(buffer, 64);
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
                                 cm::cache::CacheStore& cache,
                                 const std::string& dataDir)
    : m_mysql(mysql), m_cache(cache), m_dataDir(dataDir) {
}

ErrorCode DownloadService::openDownload(
    std::uint64_t uid,
    const cm::protocol::DownloadOpenRq& rq,
    cm::protocol::DownloadOpenRs* out) {
    if (out == NULL) {
        return ErrorCode::InvalidArgument;
    }
    *out = cm::protocol::DownloadOpenRs();

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

    cm::db::FileDao fileDao(*conn);

    // 不存在 / 非本人 / 已删除都回 1002，不泄露节点是否存在。
    cm::db::UserFileRow node;
    ErrorCode rc = fileDao.findUserFile(uid, rq.ufId, &node);
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

    cm::db::BlobRow blob;
    rc = fileDao.getBlobById(node.fid, &blob);
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
    if (blockCount == 0 || blockCount > 0xFFFFFFFFULL ||
        rq.startBlock >= blockCount) {
        out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        return ErrorCode::InvalidArgument;
    }

    const std::string ticket = makeTicket();
    const std::string value = std::to_string(node.ufId) + "|" +
                              std::to_string(blob.fid) + "|" +
                              std::to_string(uid) + "|" +
                              std::to_string(blob.size) + "|" + blob.md5;
    if (!m_cache.put(cm::cache::CacheKey::downloadTicket(ticket), value,
                     kTicketTtl)) {
        // 票据是授权凭证：Redis 写失败必须拒绝，不 fail-open。
        out->code = static_cast<std::uint32_t>(ErrorCode::RedisUnavailable);
        return ErrorCode::RedisUnavailable;
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
    out->ticketExpireMs =
        nowMs() + static_cast<std::uint64_t>(kTicketTtl.count());
    out->startBlock = rq.startBlock;
    out->zeroCopy = zeroCopyFlag();
    out->hls = (::access(hlsPath.c_str(), F_OK) == 0) ? 1 : 0;
    conn.reset();
    return ErrorCode::Ok;
}

}  // namespace logic
}  // namespace cm
