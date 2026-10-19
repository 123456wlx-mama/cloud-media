#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "cm/base/error_code.h"
#include "cm/protocol/messages/upload.h"

namespace cm {

namespace db {
class MysqlConn;
class MysqlPool;
}  // namespace db

namespace logic {

// 每连接在途分片闸门（06 §5.3：每连接最多 2 个在途分片）。
// tryBegin() 返回 false 时已经调用 pauseRead；end() 把计数降到 limit 以下时调用 resumeRead。
// 两个回调都由调用方保证在连接所属 loop 线程执行（pauseRead/resumeRead 是 loop 线程 API）。
class UploadInFlightGate {
public:
    using Action = std::function<void()>;

    UploadInFlightGate(std::size_t limit, Action pauseRead, Action resumeRead);

    UploadInFlightGate(const UploadInFlightGate&) = delete;
    UploadInFlightGate& operator=(const UploadInFlightGate&) = delete;

    // 占一个在途名额；返回 false 表示已达上限（此时连接已 pauseRead）。
    bool tryBegin();

    // 释放一个在途名额；返回 true 表示刚从“已暂停”恢复到可读。
    bool end();

    std::size_t inFlight() const;

private:
    mutable std::mutex m_mutex;
    std::size_t m_limit;
    std::size_t m_inFlight;
    bool m_paused;
    Action m_pauseRead;
    Action m_resumeRead;
};

// 10006 的业务处理：校验 → 落盘 → TransferDao::upsertChunkIfAbsent → 组 Rs。
// 计数、续期、c_state=0 覆盖、同 MD5 幂等、不同 MD5 拒绝全部由 DAO 的事务完成，
// 本类不开启事务、也不直接访问 MySQL/Redis。
class UploadChunkHandler {
public:
    // uploadRoot 是 tmp 与 blobs 的共同根（ServerConfig::dataDir）。
    UploadChunkHandler(cm::db::MysqlPool& mysql, const std::string& uploadRoot);

    UploadChunkHandler(const UploadChunkHandler&) = delete;
    UploadChunkHandler& operator=(const UploadChunkHandler&) = delete;

    // seq 是帧 SEQ，只用于临时文件名去重。
    // gate 可为 NULL；非空时在返回前无条件释放一个在途名额。
    cm::ErrorCode handle(std::uint64_t uid,
                         std::uint32_t seq,
                         const cm::protocol::UploadChunkRq& rq,
                         UploadInFlightGate* gate,
                         cm::protocol::UploadChunkRs* out);

private:
    cm::db::MysqlPool& m_mysql;
    std::string m_uploadRoot;
};

}  // namespace logic
}  // namespace cm
