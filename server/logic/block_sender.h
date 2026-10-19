#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

#include "cm/base/error_code.h"
#include "logic/download_ticket.h"

namespace cm {

namespace db {
class MysqlPool;
}  // namespace db

namespace cache {
class RedisPool;
}  // namespace cache

namespace logic {

// 缓冲块发送器（D-3，CM_DOWNLOAD_ZEROCOPY=OFF 的基线路径）：
// 校验/续期票据 → 查 blob → IO 池读块 + 流式 CRC → 构造 10011 完整帧。
//
// 线程模型：
//   - sendBlock 在业务线程调用：touch 票据（Redis）与查 blob（MySQL）都在此完成；
//   - 读块与 CRC 通过注入的 executor 在 IO 池执行（IO 线程不碰 DB/Redis）；
//   - done 在 executor 线程回调，frame 是完整帧字节；10013 没有业务 RS，
//     失败（code != Ok 且 frame 为空）由调用方走 ERROR_RS，并在连接所属 loop 发送。
// 本类不 include net，发送通过 done 回调交给调用方。
//
// 全局并发上限 16（峰值 16×4MB=64MB）；超过则排队，不阻塞调用线程。
class BlockSender {
public:
    using Task = std::function<void()>;
    // 返回 false 表示执行器已停止（任务未执行）；生产传 IO 池的 run。
    using Executor = std::function<bool(Task)>;
    using DoneCallback =
        std::function<void(cm::ErrorCode code, std::string frame)>;

    BlockSender(cm::db::MysqlPool& mysql,
                cm::cache::RedisPool& redis,
                Executor executor,
                const std::string& dataDir);

    BlockSender(const BlockSender&) = delete;
    BlockSender& operator=(const BlockSender&) = delete;

    // uid 是会话 uid，必须与 ticket 绑定的 uid 一致；uid/fid 不符回 1004。
    // blockIndex 越界回 1001；blob 缺失/读失败回 3001。
    void sendBlock(std::uint64_t uid,
                   const std::string& ticket,
                   std::uint64_t fid,
                   std::uint32_t blockIndex,
                   std::uint32_t seq,
                   DoneCallback done);

    // 在途（正在 IO 池读块）与排队中的请求数，供背压观测与测试。
    std::size_t inFlight() const;
    std::size_t pending() const;

private:
    struct WorkItem {
        std::string path;    // blob 相对路径 aa/bb/<md5>
        std::uint64_t size;  // blob 总大小（块数校验 + stat 比对）
        std::uint32_t blockIndex;
        std::uint32_t seq;
        DoneCallback done;
    };

    void enqueue(WorkItem item);
    void dispatchToPool(WorkItem item);
    void runBlock(const WorkItem& item);
    void onBlockDone();
    cm::ErrorCode readBlock(const WorkItem& item, std::string* frame) const;

    cm::db::MysqlPool& m_mysql;
    DownloadTicket m_ticket;
    Executor m_executor;
    std::string m_dataDir;

    mutable std::mutex m_mutex;
    std::size_t m_inFlight;
    std::deque<WorkItem> m_queue;
};

}  // namespace logic
}  // namespace cm
