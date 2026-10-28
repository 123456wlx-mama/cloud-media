#include "upload_service.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cm/base/md5.h"
#include "cm/log/logger.h"

#include "cache/cache_store.h"
#include "logic/block_crc.h"
#include "logic/upload_fs.h"
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

// DB 里是 UTC DATETIME(6)；连接池已 SET SESSION time_zone='+00:00'（05 §4.3）。
std::string nowUtcDatetime() {
    const std::time_t now = std::time(nullptr);
    std::tm tm;
    std::memset(&tm, 0, sizeof(tm));
    ::gmtime_r(&now, &tm);

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.000000",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buffer);
}

bool isExpired(const std::string& expireAt) {
    return !expireAt.empty() && expireAt < nowUtcDatetime();
}

// 合并时单次读写缓冲；与 06 §5.5 的 1MB 缓冲一致。
const std::size_t kMergeBufferSize = 1024 * 1024;
// 10031 的坏片列表上限；超出只返回前 1024 个。
const std::size_t kMaxBadIndexes = 1024;

bool writeAll(int fd, const char* data, std::size_t len) {
    std::size_t written = 0;
    while (written < len) {
        const ssize_t n = ::write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

std::string chunkFileName(std::uint32_t index) {
    char name[32];
    std::snprintf(name, sizeof(name), "%u.bin", index);
    return std::string(name);
}

std::uint64_t expectedChunkSize(const cm::db::TransferTaskRow& task,
                                std::uint32_t index) {
    const std::uint64_t offset =
        static_cast<std::uint64_t>(index) * task.chunkSize;
    if (offset >= task.totalSize) {
        return 0;
    }
    const std::uint64_t remain = task.totalSize - offset;
    return remain < task.chunkSize ? remain : task.chunkSize;
}

void addBadIndex(std::vector<std::uint32_t>* bad, std::uint32_t index) {
    if (bad->size() < kMaxBadIndexes) {
        bad->push_back(index);
    }
}

std::string utcNowIso8601() {
    const std::time_t now = std::time(nullptr);
    std::tm tm;
    std::memset(&tm, 0, sizeof(tm));
    ::gmtime_r(&now, &tm);

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buffer);
}

// sidecar <md5>.meta：size/md5/created_at，失败只影响诊断，不影响主流程。
bool writeBlobMeta(const std::string& blobPath,
                   std::uint64_t size,
                   const std::string& md5) {
    const std::string tmpPath = blobPath + ".meta.tmp";
    const std::string finalPath = blobPath + ".meta";

    const int fd = ::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }

    char body[160];
    const int len = std::snprintf(
        body, sizeof(body), "size=%llu\nmd5=%s\ncreated_at=%s\n",
        static_cast<unsigned long long>(size), md5.c_str(),
        utcNowIso8601().c_str());
    bool ok = len > 0 && static_cast<std::size_t>(len) < sizeof(body) &&
              writeAll(fd, body, static_cast<std::size_t>(len));
    if (ok && ::fsync(fd) != 0) {
        ok = false;
    }
    if (::close(fd) != 0) {
        ok = false;
    }
    if (!ok || ::rename(tmpPath.c_str(), finalPath.c_str()) != 0) {
        ::unlink(tmpPath.c_str());
        return false;
    }
    return true;
}

// 按 index 升序读分片并写 merged，同时更新整文件 MD5 与逐片 MD5 校验。
// 坏片只记录下标、不中断扫描；IO 失败返回 NoSpace 并删除半成品。
cm::ErrorCode mergeChunks(const std::string& dataDir,
                          const cm::db::TransferTaskRow& task,
                          const std::map<std::uint32_t, cm::db::ChunkRow>& chunks,
                          std::string* mergedPath,
                          std::string* wholeMd5,
                          std::vector<std::uint32_t>* badIndexes) {
    const std::string taskDir = dataDir + "/tmp/" + task.taskId;
    // O1：合并结果直接写进 blob 目标目录（aa/bb 由声明的 fileMd5 推导），
    // 发布阶段只剩一次同目录 rename，省掉 tmp→blob 的第二次全量写。
    // 临时名以 '.' 开头并带 taskId，避免并发任务同名冲突。
    const std::string blobDir = dataDir + "/blobs/" + task.fileMd5.substr(0, 2) +
                                "/" + task.fileMd5.substr(2, 2);
    *mergedPath = blobDir + "/." + task.taskId + ".merged.tmp";
    wholeMd5->clear();

    // 零分片的任务也要能进入逐片校验（全部回 bad_indexes），而不是直接报磁盘错误。
    if (!makeDirs(taskDir) || !makeDirs(blobDir)) {
        return cm::ErrorCode::NoSpace;
    }

    const int outFd = ::open(mergedPath->c_str(),
                             O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outFd < 0) {
        return cm::ErrorCode::NoSpace;
    }

    cm::Md5 whole;
    std::vector<char> buffer(kMergeBufferSize);
    bool ioFailure = false;

    for (std::uint32_t index = 0; index < task.totalChunks; ++index) {
        const std::map<std::uint32_t, cm::db::ChunkRow>::const_iterator it =
            chunks.find(index);
        if (it == chunks.end() || it->second.state != 1) {
            addBadIndex(badIndexes, index);  // 缺片或未确认
            continue;
        }

        const std::uint64_t expected = expectedChunkSize(task, index);
        const std::string chunkPath = taskDir + "/" + chunkFileName(index);
        struct stat st;
        if (expected == 0 || ::stat(chunkPath.c_str(), &st) != 0 ||
            static_cast<std::uint64_t>(st.st_size) != expected) {
            addBadIndex(badIndexes, index);
            continue;
        }

        const int inFd = ::open(chunkPath.c_str(), O_RDONLY);
        if (inFd < 0) {
            addBadIndex(badIndexes, index);
            continue;
        }

        cm::Md5 chunkMd5;
        bool readOk = true;
        for (;;) {
            const ssize_t n = ::read(inFd, &buffer[0], buffer.size());
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                readOk = false;
                break;
            }
            if (n == 0) {
                break;
            }
            chunkMd5.update(&buffer[0], static_cast<std::size_t>(n));
            whole.update(&buffer[0], static_cast<std::size_t>(n));
            if (!writeAll(outFd, &buffer[0], static_cast<std::size_t>(n))) {
                ioFailure = true;
                break;
            }
        }
        ::close(inFd);

        if (ioFailure) {
            break;
        }
        if (!readOk || chunkMd5.hex() != it->second.md5) {
            addBadIndex(badIndexes, index);
        }
    }

    if (::fsync(outFd) != 0) {
        ioFailure = true;
    }
    if (::close(outFd) != 0) {
        ioFailure = true;
    }

    if (ioFailure) {
        ::unlink(mergedPath->c_str());
        return cm::ErrorCode::NoSpace;
    }
    if (!badIndexes->empty()) {
        ::unlink(mergedPath->c_str());
        return cm::ErrorCode::Ok;  // 调用方按 badIndexes 决定回包
    }

    *wholeMd5 = whole.hex();
    return cm::ErrorCode::Ok;
}

}  // namespace

UploadService::UploadService(cm::db::MysqlPool& mysql,
                             cm::cache::CacheStore& cache,
                             const std::string& dataDir)
    : m_mysql(mysql), m_cache(cache), m_dataDir(dataDir) {
}

ErrorCode UploadService::init(std::uint64_t uid,
                              const cm::protocol::UploadInitRq& rq,
                              cm::protocol::UploadInitRs* out) {
    if (out == nullptr) {
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

ErrorCode UploadService::continueUpload(
    std::uint64_t uid,
    const cm::protocol::ContinueUploadRq& rq,
    cm::protocol::ContinueUploadRs* out) {
    if (out == nullptr) {
        return ErrorCode::InvalidArgument;
    }
    *out = cm::protocol::ContinueUploadRs();

    if (uid == 0 || !isHex32(rq.taskId)) {
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

    ErrorCode rc = ErrorCode::Ok;
    {
        cm::db::Transaction tx(*conn);
        if (!tx.valid()) {
            rc = (conn->lastError() == ErrorCode::Ok) ? ErrorCode::DbUnavailable
                                                      : conn->lastError();
        } else {
            rc = doContinue(*conn, uid, rq, out);
            if (rc == ErrorCode::Ok && !tx.commit()) {
                rc = (tx.lastError() == ErrorCode::Ok) ? ErrorCode::DbQueryFailed
                                                       : tx.lastError();
            }
        }
    }
    conn.reset();

    if (rc != ErrorCode::Ok) {
        if (out->code == 0) {
            out->code = cm::to_wire_code(rc);
        }
        return rc;
    }
    return ErrorCode::Ok;
}

ErrorCode UploadService::doContinue(cm::db::MysqlConn& conn,
                                    std::uint64_t uid,
                                    const cm::protocol::ContinueUploadRq& rq,
                                    cm::protocol::ContinueUploadRs* out) {
    cm::db::TransferDao transferDao(conn);

    // 不存在与非本人同样回 1004，不泄露任务存在性（06 §4.4）。
    cm::db::TransferTaskRow task;
    ErrorCode rc = transferDao.lockTaskForUpdate(uid, rq.taskId, &task);
    if (rc == ErrorCode::NotFound) {
        out->code = static_cast<std::uint32_t>(ErrorCode::PermissionDenied);
        return ErrorCode::PermissionDenied;
    }
    if (rc != ErrorCode::Ok) {
        return rc;
    }

    // 0/1/2 可续传；6 与已过期回 3007；3/4/5/7 回 3005。
    if (task.status == 6 || isExpired(task.expireAt)) {
        out->code = static_cast<std::uint32_t>(ErrorCode::TaskExpired);
        return ErrorCode::TaskExpired;
    }
    if (task.status != 0 && task.status != 1 && task.status != 2) {
        out->code = static_cast<std::uint32_t>(ErrorCode::TaskStateInvalid);
        return ErrorCode::TaskStateInvalid;
    }
    if (task.totalChunks == 0 ||
        task.totalChunks > cm::protocol::kMaxChunkCount) {
        // 位图固定 ceil(total/8)；超过 64KB 直接拒绝，不返回截断位图。
        out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        return ErrorCode::InvalidArgument;
    }

    std::vector<std::uint32_t> doneIndexes;
    rc = transferDao.listDoneIndexes(rq.taskId, &doneIndexes);
    if (rc != ErrorCode::Ok) {
        return rc;
    }

    std::uint64_t doneBytes = 0;
    rc = transferDao.sumDoneBytes(rq.taskId, &doneBytes);
    if (rc != ErrorCode::Ok) {
        return rc;
    }

    // 位图定长：ceil(total_chunks/8)，LSB-first（第 0 片是 byte0 的 bit0）。
    const std::size_t bitmapBytes =
        (static_cast<std::size_t>(task.totalChunks) + 7) / 8;
    std::string bitmap(bitmapBytes, '\0');
    std::uint32_t doneChunks = 0;
    for (std::size_t i = 0; i < doneIndexes.size(); ++i) {
        const std::uint32_t index = doneIndexes[i];
        if (index >= task.totalChunks) {
            continue;  // 脏数据不进位图，也不计数
        }
        const std::size_t pos = index / 8;
        const unsigned char bit = static_cast<unsigned char>(1u << (index % 8));
        bitmap[pos] = static_cast<char>(
            static_cast<unsigned char>(bitmap[pos]) | bit);
        ++doneChunks;
    }

    // t_chunk 是唯一事实源，计数缓存漂移时以重算值为准，并在同一事务内修正。
    if (task.doneChunks != doneChunks || task.offset != doneBytes) {
        // 计数缓存漂移：已按 t_chunk 重算修正，但漂移本身说明有写入路径漏了同步。
        Logger::instance().log(
            LogLevel::Warn, __FILE__, __LINE__,
            "upload counters drift (task=" + rq.taskId +
                ", doneChunks " + std::to_string(task.doneChunks) + "->" +
                std::to_string(doneChunks) + ", offset " +
                std::to_string(task.offset) + "->" +
                std::to_string(doneBytes) + "), recalculated from t_chunk");
        rc = transferDao.syncDoneCounters(rq.taskId, doneChunks, doneBytes);
        if (rc != ErrorCode::Ok) {
            return rc;
        }
    }

    out->code = 0;
    out->taskId = rq.taskId;
    out->status = task.status;
    out->chunkSize = task.chunkSize;
    out->totalChunks = task.totalChunks;
    out->doneChunks = doneChunks;
    out->doneBytes = doneBytes;
    out->bitmap = bitmap;
    out->expireAtMs = task.expireAtMs;
    return ErrorCode::Ok;
}

ErrorCode UploadService::completeUpload(
    std::uint64_t uid,
    const cm::protocol::UploadCompleteRq& rq,
    cm::protocol::UploadCompleteRs* out) {
    if (out == nullptr) {
        return ErrorCode::InvalidArgument;
    }
    *out = cm::protocol::UploadCompleteRs();

    if (uid == 0 || !isHex32(rq.taskId) || m_dataDir.empty()) {
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
    cm::db::TransferDao transferDao(*conn);

    cm::db::TransferTaskRow task;
    ErrorCode rc = transferDao.findTaskByTaskId(rq.taskId, &task);
    if (rc == ErrorCode::NotFound) {
        out->code = static_cast<std::uint32_t>(ErrorCode::TaskNotFound);
        return ErrorCode::TaskNotFound;
    }
    if (rc != ErrorCode::Ok) {
        out->code = static_cast<std::uint32_t>(cm::to_wire_code(rc));
        return rc;
    }
    if (task.userId != uid) {
        // 不存在与非本人同样回 3004，不泄露任务存在性。
        out->code = static_cast<std::uint32_t>(ErrorCode::TaskNotFound);
        return ErrorCode::TaskNotFound;
    }

    // 已完成：直接回放结果，不重复合并、不重复加引用计数。
    if (task.status == 4) {
        out->code = 0;
        out->fileMd5 = task.fileMd5;
        out->size = task.totalSize;
        out->fileName = task.fileName;

        cm::db::BlobRow blob;
        if (fileDao.findBlobByMd5(task.fileMd5, &blob) == ErrorCode::Ok) {
            out->fileId = blob.fid;
            cm::db::UserFileRow node;
            if (fileDao.findActiveNodeByFid(uid, task.parentUfId, blob.fid,
                                            &node) == ErrorCode::Ok) {
                out->ufId = node.ufId;
                out->fileName = node.name;
            }
        }
        conn.reset();
        return ErrorCode::Ok;
    }

    // 选主：status 1→7。抢不到（含正在合并/已取消/过期）一律 3004。
    bool elected = false;
    rc = transferDao.electMerge(uid, rq.taskId, &elected);
    if (rc != ErrorCode::Ok) {
        out->code = static_cast<std::uint32_t>(cm::to_wire_code(rc));
        return rc;
    }
    if (!elected) {
        out->code = static_cast<std::uint32_t>(ErrorCode::TaskNotFound);
        return ErrorCode::TaskNotFound;
    }

    // 选主成功后任何失败都回 1（可重试），不把 7 留成不可重试的终态；
    // 7 只表示“合并过程中进程崩溃”，由启动恢复 7→2 兜底。
    std::function<void()> returnToUploading = [&]() {
        std::vector<std::uint8_t> from(1, 7);
        bool changed = false;
        if (transferDao.markStatus(uid, rq.taskId, from, 1, 0, &changed) !=
            ErrorCode::Ok) {
            // 停留在 7 的任务只能靠进程重启后的 7->2 恢复，必须留痕。
            Logger::instance().log(
                LogLevel::Warn, __FILE__, __LINE__,
                "markStatus 7->1 failed, task stuck in merging (task=" +
                    rq.taskId + ")");
        }
    };

    std::vector<cm::db::ChunkRow> chunkRows;
    rc = transferDao.listChunks(rq.taskId, &chunkRows);
    if (rc != ErrorCode::Ok) {
        returnToUploading();
        out->code = static_cast<std::uint32_t>(cm::to_wire_code(rc));
        return rc;
    }
    std::map<std::uint32_t, cm::db::ChunkRow> chunks;
    for (std::size_t i = 0; i < chunkRows.size(); ++i) {
        chunks[chunkRows[i].index] = chunkRows[i];
    }

    // 文件 IO 全部在事务外：合并、逐片校验、整文件 MD5。
    std::string mergedPath;
    std::string wholeMd5;
    std::vector<std::uint32_t> badIndexes;
    rc = mergeChunks(m_dataDir, task, chunks, &mergedPath, &wholeMd5,
                     &badIndexes);
    if (rc != ErrorCode::Ok) {
        // 保留 tmp 供重试；任务回 1。
        returnToUploading();
        out->code = static_cast<std::uint32_t>(cm::to_wire_code(rc));
        return rc;
    }

    // 坏片：7→1 允许重传；merged 已由 mergeChunks 删除。
    if (!badIndexes.empty()) {
        returnToUploading();
        out->code = static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
        out->badIndexes = badIndexes;
        return ErrorCode::InvalidArgument;
    }

    // 整文件 MD5 与客户端声明不符：7→1，客户端重新 init。
    if (wholeMd5 != task.fileMd5) {
        // O1 后合并文件落在 blobs 目录里，必须就地清掉，避免留下隐藏残留。
        ::unlink(mergedPath.c_str());
        returnToUploading();
        out->code = static_cast<std::uint32_t>(ErrorCode::Md5Mismatch);
        return ErrorCode::Md5Mismatch;
    }

    const std::string relPath =
        wholeMd5.substr(0, 2) + "/" + wholeMd5.substr(2, 2) + "/" + wholeMd5;
    const std::string blobDir = m_dataDir + "/blobs/" + wholeMd5.substr(0, 2) +
                                "/" + wholeMd5.substr(2, 2);
    const std::string blobPath = blobDir + "/" + wholeMd5;

    cm::db::BlobRow existingBlob;
    bool blobExists = false;
    if (fileDao.findBlobByMd5(wholeMd5, &existingBlob) == ErrorCode::Ok &&
        existingBlob.state == 1) {
        blobExists = true;
    }

    if (blobExists) {
        // 已有同内容 blob：只落节点与计数，合并文件丢弃。
        ::unlink(mergedPath.c_str());
    } else if (::access(blobPath.c_str(), F_OK) == 0) {
        // 并发秒传已把文件放到位：丢弃本次合并文件。
        ::unlink(mergedPath.c_str());
    } else {
        if (!makeDirs(blobDir)) {
            returnToUploading();
            out->code = static_cast<std::uint32_t>(ErrorCode::NoSpace);
            return ErrorCode::NoSpace;
        }
        if (::rename(mergedPath.c_str(), blobPath.c_str()) != 0) {
            // 保留 merged 与 tmp，任务回 1，客户端可立即重试。
            returnToUploading();
            out->code = static_cast<std::uint32_t>(ErrorCode::NoSpace);
            return ErrorCode::NoSpace;
        }
    }

    // 合并落库事务：事务内无文件 IO。
    std::uint64_t fileId = 0;
    std::uint64_t ufId = 0;
    std::string finalName;
    {
        cm::db::Transaction tx(*conn);
        if (!tx.valid()) {
            rc = (conn->lastError() == ErrorCode::Ok) ? ErrorCode::DbUnavailable
                                                      : conn->lastError();
        } else {
            cm::db::BlobWriteKind kind = cm::db::BlobWriteKind::Inserted;
            rc = fileDao.insertBlobOnDuplicateIncrement(
                wholeMd5, task.totalSize, relPath, &fileId, &kind);
            if (rc == ErrorCode::Ok) {
                rc = fileDao.insertUserFileAutoRenameInTx(
                    uid, task.parentUfId, fileId, task.fileName, &ufId,
                    &finalName);
            }
            if (rc == ErrorCode::Ok) {
                rc = transferDao.syncDoneCounters(rq.taskId, task.totalChunks,
                                                  task.totalSize);
            }
            if (rc == ErrorCode::Ok) {
                std::vector<std::uint8_t> from(1, 7);
                bool changed = false;
                rc = transferDao.markStatus(uid, rq.taskId, from, 4, 0,
                                            &changed);
                if (rc == ErrorCode::Ok && !changed) {
                    rc = ErrorCode::TaskNotFound;
                }
            }
            if (rc == ErrorCode::Ok && !tx.commit()) {
                rc = (tx.lastError() == ErrorCode::Ok) ? ErrorCode::DbQueryFailed
                                                       : tx.lastError();
            }
        }
    }

    if (rc != ErrorCode::Ok) {
        // 已 rename 的 blob 不删（避免误删他人数据），留给孤儿 GC 对账。
        Logger::instance().log(
            LogLevel::Warn, __FILE__, __LINE__,
            "upload commit failed, blob left for orphan GC (md5=" +
                task.fileMd5 + ", path=" + relPath +
                ", err=" + std::to_string(static_cast<int>(rc)) + ")");
        returnToUploading();
        out->code = (rc == ErrorCode::TaskNotFound)
                        ? static_cast<std::uint32_t>(ErrorCode::TaskNotFound)
                        : static_cast<std::uint32_t>(cm::to_wire_code(rc));
        return rc;
    }

    // sidecar 失败只 WARN，不影响上传结果。
    if (!writeBlobMeta(blobPath, task.totalSize, wholeMd5)) {
        Logger::instance().log(
            LogLevel::Warn, __FILE__, __LINE__,
            "writeBlobMeta failed (path=" + blobPath + ", size=" +
                std::to_string(task.totalSize) + ")");
    }
    // 块 CRC sidecar 改为懒生成（O7-c）：上传只落 blob + .meta，
    // 首个下载请求由 ensureBlockCrc（sendfile_block_sender）按块补算并写回。
    // 收益：省掉「合并完成后再把整个 blob 读一遍算 CRC」的那趟 10MB 读 + 16% CPU。

    // 成功后清理本任务临时目录（分片 + merged 临时文件）。
    removeTree(m_dataDir + "/tmp/" + rq.taskId);

    out->code = 0;
    out->fileId = fileId;
    out->ufId = ufId;
    out->fileName = finalName;
    out->fileMd5 = wholeMd5;
    out->size = task.totalSize;
    conn.reset();
    return ErrorCode::Ok;
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
        char* end = nullptr;
        const unsigned long long value =
            std::strtoull(cachedFid.c_str(), &end, 10);
        if (end != nullptr && *end == '\0' && value != 0) {
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
        if (rc == ErrorCode::Ok) {
            out->result = 1;
            out->fileId = fileId;
            out->ufId = ufId;
            out->fileName = finalName;
            out->chunkSize = kChunkSize;
            out->totalChunks = totalChunks;
            out->expireAtMs = 0;
            return ErrorCode::Ok;
        }
        // NotFound：候选 blob 在「命中判定」与「落库」之间被孤儿 GC 回收。
        // 不能当成功返回，也不能直接报错——继续走下面的真实上传流程自愈。
        if (rc != ErrorCode::NotFound) {
            return rc;
        }
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
