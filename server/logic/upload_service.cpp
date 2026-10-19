#include "upload_service.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <string>

#include "cm/base/md5.h"

#include "cache/cache_store.h"
#include "db/file_dao.h"
#include "db/mysql_conn.h"
#include "db/mysql_pool.h"
#include "db/transfer_dao.h"

namespace cm {
namespace logic {
namespace {

const std::uint32_t kChunkSize = 4194304;                 // 固定 4MB（06 §4.1）
const std::uint64_t kMaxFileSize = 100ULL * 1024 * 1024 * 1024;  // 100GB（附录 B）
const std::uint32_t kTaskTtlSeconds = 86400;              // 24h（06 §5.7）
const std::chrono::milliseconds kReqResultTtl(10 * 60 * 1000);  // req 幂等结果 10min
const std::chrono::milliseconds kReqLockTtl(5000);              // 短锁 5s
const std::chrono::milliseconds kFileMd5Ttl(60 * 60 * 1000);    // 秒传索引 60min

bool isHex32(const std::string& value) {
    if (value.size() != 32) {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        const bool digit = (c >= '0' && c <= '9');
        const bool lower = (c >= 'a' && c <= 'f');
        const bool upper = (c >= 'A' && c <= 'F');
        if (!digit && !lower && !upper) {
            return false;
        }
    }
    return true;
}

// task_id = md5(纳秒时间 + 进程内序号 + pid)，32 位小写 hex（协议要求）。
std::string makeTaskId() {
    static std::atomic<std::uint64_t> counter(0);

    const std::uint64_t nowNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const std::uint64_t seq = counter.fetch_add(1, std::memory_order_relaxed);

    char seed[96];
    std::snprintf(seed, sizeof(seed), "%ld:%llu:%llu",
                  static_cast<long>(::getpid()),
                  static_cast<unsigned long long>(nowNs),
                  static_cast<unsigned long long>(seq));
    return cm::md5Hex(std::string(seed));
}

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

}  // namespace

UploadService::UploadService(cm::db::MysqlPool& mysql,
                             cm::cache::CacheStore& cache)
    : m_mysql(mysql), m_cache(cache) {
}

ErrorCode UploadService::init(std::uint64_t uid,
                              const cm::protocol::UploadInitRq& rq,
                              cm::protocol::UploadInitRs* out) {
    if (out == NULL) {
        return ErrorCode::InvalidArgument;
    }
    *out = cm::protocol::UploadInitRs();

    // 1) 参数校验：md5 32 hex、size 在 (0, 100GB]、文件名 1~255 字节。
    if (uid == 0 || rq.fileName.empty() || rq.fileName.size() > 255 ||
        rq.totalSize == 0 || rq.totalSize > kMaxFileSize ||
        !isHex32(rq.fileMd5)) {
        out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        return ErrorCode::InvalidArgument;
    }

    // 客户端建议值一律忽略，服务端按固定 4MB 计算（06 §4.1）。
    const std::uint64_t totalChunks64 =
        (rq.totalSize + kChunkSize - 1) / kChunkSize;
    if (totalChunks64 == 0 || totalChunks64 > cm::protocol::kMaxChunkCount) {
        out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        return ErrorCode::InvalidArgument;
    }
    const std::uint32_t totalChunks = static_cast<std::uint32_t>(totalChunks64);

    // 2) req 幂等：命中直接回放 10005。
    const std::string resultKey = cm::cache::CacheKey::uploadReq(uid, rq.reqId);
    std::string cached;
    if (m_cache.get(resultKey, &cached) == cm::cache::CacheStore::Result::Hit) {
        if (out->decode(cached)) {
            return ErrorCode::Ok;
        }
        // 缓存内容解析失败视为脏数据，继续走完整流程（下次 put 会覆盖）。
    }

    const std::string lockKey = resultKey + ":lock";
    const cm::cache::CacheStore::LockResult lock =
        m_cache.tryLock(lockKey, "1", kReqLockTtl);
    if (lock == cm::cache::CacheStore::LockResult::Busy) {
        // 另一个请求正在处理同一个 req_id，按 06 §4.1 回 1006 让客户端退避。
        out->code = static_cast<std::uint32_t>(ErrorCode::Timeout);
        return ErrorCode::Timeout;
    }
    // Error：Redis 不可用 → fail-open，不持锁直接走 DB。

    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        if (lock == cm::cache::CacheStore::LockResult::Acquired) {
            m_cache.invalidate(lockKey);
        }
        const ErrorCode code = (m_mysql.lastError() == ErrorCode::Ok)
                                   ? ErrorCode::DbPoolTimeout
                                   : m_mysql.lastError();
        out->code = static_cast<std::uint32_t>(code);
        return code;
    }

    const ErrorCode rc = doInit(*conn, uid, rq, totalChunks, out);
    conn.reset();  // 先归还连接，再写缓存，缩短占用时间

    if (rc != ErrorCode::Ok) {
        out->code = static_cast<std::uint32_t>(rc);
    } else {
        std::string payload;
        if (out->encode(&payload)) {
            m_cache.put(resultKey, payload, kReqResultTtl);
        }
    }

    if (lock == cm::cache::CacheStore::LockResult::Acquired) {
        m_cache.invalidate(lockKey);
    }
    return rc;
}

ErrorCode UploadService::doInit(cm::db::MysqlConn& conn,
                                std::uint64_t uid,
                                const cm::protocol::UploadInitRq& rq,
                                std::uint32_t totalChunks,
                                cm::protocol::UploadInitRs* out) {
    cm::db::FileDao fileDao(conn);
    cm::db::TransferDao transferDao(conn);

    // 1) parent 必须属于当前用户、未删除、且是目录。
    if (rq.parentId != 0) {
        cm::db::UserFileRow parent;
        const ErrorCode rc = fileDao.findUserFile(uid, rq.parentId, &parent);
        if (rc == ErrorCode::NotFound) {
            return ErrorCode::NotFound;
        }
        if (rc != ErrorCode::Ok) {
            return rc;
        }
        if (parent.deletedAt != cm::db::FileDao::activeDeletedAt()) {
            return ErrorCode::NotFound;
        }
        if (!parent.isDir) {
            return ErrorCode::InvalidArgument;
        }
    }

    // 2) 秒传：Redis 索引 → DB uk_f_md5；只认 f_state=1 且 size 一致。
    const std::string md5Key = cm::cache::CacheKey::fileByMd5(rq.fileMd5);
    std::uint64_t hitFid = 0;

    std::string cachedFid;
    if (m_cache.get(md5Key, &cachedFid) == cm::cache::CacheStore::Result::Hit) {
        char* end = NULL;
        const unsigned long long value =
            std::strtoull(cachedFid.c_str(), &end, 10);
        if (end != NULL && *end == '\0' && value != 0) {
            cm::db::BlobRow blob;
            if (fileDao.getBlobById(value, &blob) == ErrorCode::Ok &&
                blob.state == 1 && blob.size == rq.totalSize) {
                hitFid = blob.fid;
            }
        }
    }

    if (hitFid == 0) {
        cm::db::BlobRow blob;
        if (fileDao.findBlobByMd5(rq.fileMd5, &blob) == ErrorCode::Ok &&
            blob.state == 1 && blob.size == rq.totalSize) {
            hitFid = blob.fid;
            // 回填缓存；失败只影响下一次命中率，不影响本次结果。
            m_cache.put(md5Key, std::to_string(hitFid), kFileMd5Ttl);
        }
    }

    if (hitFid != 0) {
        std::uint64_t fileId = 0;
        std::uint64_t ufId = 0;
        std::string finalName;
        bool reused = false;
        const ErrorCode rc = fileDao.materializeInstantUpload(
            uid, rq.parentId, rq.fileMd5, rq.totalSize, rq.fileName,
            &fileId, &ufId, &finalName, &reused);
        if (rc != ErrorCode::Ok) {
            return rc;
        }

        out->result = 1;
        out->fileId = fileId;
        out->ufId = ufId;
        out->fileName = finalName;
        out->chunkSize = kChunkSize;
        out->totalChunks = totalChunks;
        out->expireAtMs = 0;
        return ErrorCode::Ok;
    }

    // 3) 未命中：建上传任务（status=0，expire=now+24h，由 DAO 在 SQL 内计算）。
    cm::db::NewTransferTask task;
    task.taskId = makeTaskId();
    task.userId = uid;
    task.type = 1;
    task.fileMd5 = rq.fileMd5;
    task.fileName = rq.fileName;
    task.totalSize = rq.totalSize;
    task.chunkSize = kChunkSize;
    task.totalChunks = totalChunks;
    task.parentUfId = rq.parentId;
    task.ttlSeconds = kTaskTtlSeconds;

    std::uint64_t trId = 0;
    const ErrorCode rc = transferDao.createTask(task, &trId);
    if (rc != ErrorCode::Ok) {
        return rc;
    }

    out->result = 0;
    out->taskId = task.taskId;
    out->chunkSize = kChunkSize;
    out->totalChunks = totalChunks;
    out->fileName = rq.fileName;
    out->expireAtMs = nowMs() + static_cast<std::uint64_t>(kTaskTtlSeconds) * 1000ULL;
    return ErrorCode::Ok;
}

}  // namespace logic
}  // namespace cm
