#include "logic/upload_chunk_handler.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

#include "cm/base/hex.h"
#include "cm/log/logger.h"
#include "db/mysql_pool.h"
#include "db/transfer_dao.h"

namespace cm {
namespace logic {
namespace {

const std::uint32_t kNextHintDone = 0xFFFFFFFFu;

// 临时文件名里的进程内序号：帧 SEQ 可能被复用，再加一个自增保证同进程唯一。
std::atomic<std::uint64_t> g_tmpCounter(0);

cm::ErrorCode fail(cm::protocol::UploadChunkRs* out, cm::ErrorCode code) {
    out->code = static_cast<std::uint32_t>(code);
    return code;
}

// 只创建目录（不处理文件）；已存在视为成功。
bool makeDirs(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::string current;
    current.reserve(path.size());
    for (std::size_t i = 0; i < path.size(); ++i) {
        current.push_back(path[i]);
        if (path[i] != '/' || current.size() == 1) {
            continue;
        }
        if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
            return false;
        }
    }
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

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

// 写 tmp/<task>/<index>.bin.tmp.<seq>.<n> → fsync → rename 成 <index>.bin（同目录原子）。
bool writeChunkFile(const std::string& dir,
                    std::uint32_t index,
                    std::uint32_t seq,
                    const std::string& data,
                    std::string* absolutePath) {
    if (!makeDirs(dir)) {
        return false;
    }

    char name[96];
    std::snprintf(name, sizeof(name), "/%u.bin.tmp.%u.%llu", index, seq,
                  static_cast<unsigned long long>(
                      g_tmpCounter.fetch_add(1, std::memory_order_relaxed)));
    const std::string tmpPath = dir + name;

    std::snprintf(name, sizeof(name), "/%u.bin", index);
    const std::string target = dir + name;

    const int fd = ::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }

    bool ok = writeAll(fd, data.data(), data.size());
    if (ok && ::fsync(fd) != 0) {
        ok = false;
    }
    if (::close(fd) != 0) {
        ok = false;
    }
    if (ok && ::rename(tmpPath.c_str(), target.c_str()) != 0) {
        ok = false;
    }
    if (!ok) {
        ::unlink(tmpPath.c_str());
        return false;
    }

    *absolutePath = target;
    return true;
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

// c_path 存相对路径（schema 约定：tmp/<task_id>/<index>）。
std::string chunkRelPath(const std::string& taskId, std::uint32_t index) {
    char name[32];
    std::snprintf(name, sizeof(name), "%u.bin", index);
    return std::string("tmp/") + taskId + "/" + name;
}

// 注意语义：返回的是「刚确认的这一片的下一片」，不是「下一个还没传的片」。
// 并发多连接乱序上传时两者并不相等；客户端若按它顺序补片只会低效，不会出错。
// 全部完成时返回 kNextHintDone。真正的缺口要由续传位图（10029）回答。
std::uint32_t nextHint(std::uint32_t index,
                       std::uint32_t doneChunks,
                       std::uint32_t totalChunks) {
    if (totalChunks == 0 || doneChunks >= totalChunks) {
        return kNextHintDone;
    }
    const std::uint32_t next = index + 1;
    return next < totalChunks ? next : totalChunks - 1;
}

// 无论从哪个分支返回，都释放一个在途名额。
class GateRelease {
public:
    explicit GateRelease(UploadInFlightGate* gate) : m_gate(gate) {}
    ~GateRelease() {
        if (m_gate != nullptr) {
            m_gate->end();
        }
    }

private:
    GateRelease(const GateRelease&) = delete;
    GateRelease& operator=(const GateRelease&) = delete;

    UploadInFlightGate* m_gate;
};

}  // namespace

UploadInFlightGate::UploadInFlightGate(std::size_t limit,
                                       Action pauseRead,
                                       Action resumeRead)
    : m_limit(limit == 0 ? 1 : limit),
      m_inFlight(0),
      m_paused(false),
      m_pauseRead(pauseRead),
      m_resumeRead(resumeRead) {
}

bool UploadInFlightGate::tryBegin() {
    bool acquired = false;
    bool shouldPause = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_inFlight < m_limit) {
            ++m_inFlight;
            acquired = true;
        } else if (!m_paused) {
            m_paused = true;
            shouldPause = true;
        }
    }
    if (shouldPause && m_pauseRead) {
        m_pauseRead();
    }
    return acquired;
}

bool UploadInFlightGate::end() {
    bool shouldResume = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_inFlight > 0) {
            --m_inFlight;
        }
        if (m_paused && m_inFlight < m_limit) {
            m_paused = false;
            shouldResume = true;
        }
    }
    if (shouldResume && m_resumeRead) {
        m_resumeRead();
    }
    return shouldResume;
}

std::size_t UploadInFlightGate::inFlight() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_inFlight;
}

UploadChunkHandler::UploadChunkHandler(cm::db::MysqlPool& mysql,
                                       const std::string& uploadRoot)
    : m_mysql(mysql), m_uploadRoot(uploadRoot) {
}

ErrorCode UploadChunkHandler::handle(std::uint64_t uid,
                                     std::uint32_t seq,
                                     const cm::protocol::UploadChunkRq& rq,
                                     UploadInFlightGate* gate,
                                     cm::protocol::UploadChunkRs* out) {
    GateRelease release(gate);
    if (out == nullptr) {
        return ErrorCode::InvalidArgument;
    }
    *out = cm::protocol::UploadChunkRs();

    if (uid == 0 || rq.taskId.size() != 32 || rq.chunkMd5.size() != 32 ||
        rq.data.empty()) {
        return fail(out, ErrorCode::InvalidArgument);
    }

    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        const ErrorCode code = (m_mysql.lastError() == ErrorCode::Ok)
                                   ? ErrorCode::DbPoolTimeout
                                   : m_mysql.lastError();
        return fail(out, code);
    }

    cm::db::TransferDao dao(*conn);
    cm::db::TransferTaskRow task;
    ErrorCode rc = dao.findTaskByTaskId(rq.taskId, &task);
    if (rc == ErrorCode::NotFound) {
        return fail(out, ErrorCode::TaskNotFound);
    }
    if (rc != ErrorCode::Ok) {
        return fail(out, rc);
    }
    if (task.userId != uid) {
        // 不存在与不属于同一用户同样回 3004，不泄露任务存在性。
        return fail(out, ErrorCode::TaskNotFound);
    }
    if (task.status != 0 && task.status != 1 && task.status != 2) {
        return fail(out, ErrorCode::TaskStateInvalid);
    }
    if (isExpired(task.expireAt)) {
        return fail(out, ErrorCode::TaskExpired);
    }
    if (rq.chunkIndex >= task.totalChunks) {
        return fail(out, ErrorCode::InvalidArgument);
    }
    if (rq.offset !=
        static_cast<std::uint64_t>(rq.chunkIndex) * task.chunkSize) {
        return fail(out, ErrorCode::InvalidArgument);
    }
    const std::uint64_t expected = expectedChunkSize(task, rq.chunkIndex);
    if (expected == 0 || rq.data.size() != expected) {
        return fail(out, ErrorCode::InvalidArgument);
    }

    // 分片 MD5 不再在收片时重算（O7-a）：传输完整性已由帧 CRC（Codec::verifyFrameCrc）
    // 覆盖，内容完整性由 complete 阶段 mergeChunks 逐片重算 + 整文件 MD5 兜底，
    // 坏片会以 badIndexes 返回让客户端重传。这里只保留长度校验。
    if (rq.chunkMd5.size() != 32) {
        return fail(out, ErrorCode::InvalidArgument);
    }
    // 统一小写：合并阶段用 Md5::hex()（恒为小写）与库里的值比对，
    // 大写录入会让该片永远判为坏片，客户端怎么重传都修不好。
    const std::string chunkMd5 = cm::toLowerHex(rq.chunkMd5);

    // 已确认且内容不同：先拒绝，避免把好分片覆盖成坏内容；
    // 数字层面的三分支判定仍由 DAO 事务负责。
    bool alreadyConfirmed = false;
    cm::db::ChunkRow existing;
    rc = dao.findChunk(rq.taskId, rq.chunkIndex, &existing);
    if (rc == ErrorCode::Ok && existing.state == 1) {
        if (existing.md5 != chunkMd5) {
            return fail(out, ErrorCode::Md5Mismatch);
        }
        alreadyConfirmed = true;
    } else if (rc != ErrorCode::Ok && rc != ErrorCode::NotFound) {
        return fail(out, rc);
    }

    const std::string relPath = chunkRelPath(task.taskId, rq.chunkIndex);
    if (!alreadyConfirmed) {
        std::string absolutePath;
        if (!writeChunkFile(m_uploadRoot + "/tmp/" + task.taskId,
                            rq.chunkIndex, seq, rq.data, &absolutePath)) {
            return fail(out, ErrorCode::FileGone);
        }
    }

    cm::db::ChunkWrite chunk;
    chunk.index = rq.chunkIndex;
    chunk.offset = rq.offset;
    chunk.size = static_cast<std::uint32_t>(rq.data.size());
    chunk.md5 = chunkMd5;
    chunk.path = relPath;

    cm::db::ChunkUpsertResult result;
    rc = dao.upsertChunkIfAbsent(uid, rq.taskId, chunk, &result);
    if (rc != ErrorCode::Ok) {
        if (!alreadyConfirmed) {
            // 分片已经落盘但没写进 t_chunk：留下的是无人引用的临时分片，
            // 只能等 expiry_job 按任务目录清理。不留痕就无法解释磁盘占用从哪来。
            Logger::instance().log(
                LogLevel::Warn, __FILE__, __LINE__,
                "upsertChunkIfAbsent failed after chunk file written (task=" +
                    rq.taskId + ", index=" + std::to_string(rq.chunkIndex) +
                    ", err=" + std::to_string(static_cast<int>(rc)) + ")");
        }
        return fail(out, rc);
    }

    out->code = 0;
    out->chunkIndex = rq.chunkIndex;
    out->duplicate = (result.kind == cm::db::ChunkWriteKind::NoOp) ? 1 : 0;
    out->doneChunks = result.doneChunks;
    out->doneBytes = result.doneBytes;
    out->nextHint = nextHint(rq.chunkIndex, result.doneChunks, result.totalChunks);
    return ErrorCode::Ok;
}

}  // namespace logic
}  // namespace cm
