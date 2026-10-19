#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cm/base/error_code.h"

namespace cm {
namespace db {

class MysqlConn;

struct TransferTaskRow {
    std::uint64_t trId;   // 自增主键，仅内部使用
    std::string taskId;   // CHAR(32) hex
    std::uint64_t userId;
    std::uint8_t type;  // 1上传 2转存 3秒传
    std::string fileMd5;  // 可为空
    std::string fileName;
    std::uint64_t totalSize;
    std::uint32_t chunkSize;
    std::uint32_t totalChunks;
    std::uint32_t doneChunks;
    std::uint64_t offset;  // tr_offset：已完成分片字节总和（派生值）
    std::uint64_t parentUfId;
    std::uint8_t status;
    std::int32_t errorCode;
    std::string expireAt;  // DATETIME(6) 文本（UTC）
    std::uint64_t expireAtMs;  // tr_expire_at 的 Unix 毫秒，由 DB 按 UTC 计算
    std::string createdAt;
    std::string updatedAt;

    TransferTaskRow();
};

struct NewTransferTask {
    std::string taskId;  // 32 hex
    std::uint64_t userId;
    std::uint8_t type;
    std::string fileMd5;  // 空串写 NULL
    std::string fileName;
    std::uint64_t totalSize;
    std::uint32_t chunkSize;
    std::uint32_t totalChunks;
    std::uint64_t parentUfId;
    std::uint32_t ttlSeconds;  // 默认 86400（24h，见 06 §5.7）

    NewTransferTask();
};

struct ChunkRow {
    std::uint32_t index;
    std::uint64_t offset;
    std::uint32_t size;
    std::string md5;
    std::uint8_t state;
    std::string path;

    ChunkRow();
};

struct ChunkWrite {
    std::uint32_t index;
    std::uint64_t offset;
    std::uint32_t size;
    std::string md5;
    std::string path;  // 相对临时目录

    ChunkWrite();
};

// 分片 upsert 的三种结局：
// Inserted = 新行；Updated = 覆盖 c_state=0 的半成品行；NoOp = 已确认且 MD5 一致的重放。
enum class ChunkWriteKind {
    Inserted = 0,
    Updated = 1,
    NoOp = 2
};

struct ChunkUpsertResult {
    ChunkWriteKind kind;
    std::uint32_t doneChunks;
    std::uint64_t doneBytes;
    std::uint32_t totalChunks;
    std::uint8_t status;

    ChunkUpsertResult();
};

// t_transfer_task 与 t_chunk 的 DAO。不持有连接池，调用方用 MysqlPool::acquire 取得连接。
// 所有 SQL 均为预处理语句；状态变更一律条件 UPDATE，用 affected_rows 判断是否抢到。
class TransferDao {
public:
    explicit TransferDao(MysqlConn& conn);

    TransferDao(const TransferDao&) = delete;
    TransferDao& operator=(const TransferDao&) = delete;

    ErrorCode createTask(const NewTransferTask& task, std::uint64_t* trId);

    ErrorCode findTaskByTaskId(const std::string& taskId, TransferTaskRow* out);

    // 必须在调用方已开启的 Transaction 内调用；事务结束才释放行锁。
    // 加锁顺序固定为「任务行 → 分片行」，与 upsertChunkIfAbsent 内部一致。
    ErrorCode lockTaskForUpdate(std::uint64_t uid,
                                const std::string& taskId,
                                TransferTaskRow* out);

    // 自带事务：锁任务行 → 读写分片行 → 只在“新完成”时 +1 计数并累加 c_size。
    // 已确认且 MD5 不同返回 Md5Mismatch(3003)，不覆盖已确认分片。
    // 任务态只接受 0/1/2（5.1 允许 Paused 继续上传）；其余返回 TaskStateInvalid(3005)。
    ErrorCode upsertChunkIfAbsent(std::uint64_t uid,
                                  const std::string& taskId,
                                  const ChunkWrite& chunk,
                                  ChunkUpsertResult* out);

    // 读单个分片行（只读，不加锁）；不存在返回 NotFound。
    ErrorCode findChunk(const std::string& taskId,
                        std::uint32_t index,
                        ChunkRow* out);

    // c_index 升序；LSB-first 位图由服务层组装。
    ErrorCode listDoneIndexes(const std::string& taskId,
                              std::vector<std::uint32_t>* out);

    ErrorCode sumDoneBytes(const std::string& taskId, std::uint64_t* bytes);

    // 用 t_chunk 重算出的值修正计数缓存（tr_done_chunks / tr_offset 都是派生值）。
    // 不加锁、不开事务：调用方必须已在事务内 lockTaskForUpdate 后再调用。
    ErrorCode syncDoneCounters(const std::string& taskId,
                               std::uint32_t doneChunks,
                               std::uint64_t doneBytes);

    // 条件 UPDATE：只有当前状态在 allowedFrom 内才改，changed 反映 affected_rows。
    ErrorCode markStatus(std::uint64_t uid,
                         const std::string& taskId,
                         const std::vector<std::uint8_t>& allowedFrom,
                         std::uint8_t newStatus,
                         std::int32_t errorCode,
                         bool* changed);

    // status 1→7 选主：affected_rows=1 的调用方负责合并。
    ErrorCode electMerge(std::uint64_t uid, const std::string& taskId, bool* elected);

    // 服务启动时调用：合并中断的任务 7→2（回到 Paused），不处理其他状态。
    ErrorCode resetMergingOnStartup(std::uint64_t* affected);

    // 只扫 tr_status IN (0,1,2) 且 tr_expire_at < NOW(6)，按过期时间升序。
    ErrorCode findExpired(std::vector<TransferTaskRow>* out,
                          std::size_t limit = 200);

private:
    MysqlConn& m_conn;
};

}  // namespace db
}  // namespace cm
