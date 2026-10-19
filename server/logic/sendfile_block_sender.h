#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "cm/base/error_code.h"

#if defined(CM_DOWNLOAD_ZEROCOPY) && (CM_DOWNLOAD_ZEROCOPY)

#include "logic/download_ticket.h"

namespace cm {

namespace db {
class MysqlPool;
}  // namespace db

namespace cache {
class RedisPool;
}  // namespace cache

namespace logic {

// sendfile 零拷贝块发送器（D-4，仅 CM_DOWNLOAD_ZEROCOPY=1 时编译）。
// 线程模型：
//   - sendBlock 在业务线程调用：touch 票据（Redis）、查 blob（MySQL）、
//     读 sidecar 块 CRC（readBlockCrc）都在此完成；
//   - sidecar 缺失/该块 crc==0 时投 IO 池 ensureBlockCrc 懒填充（读块算 CRC pwrite）；
//   - 懒填充失败 → 调用注入的 fallback 委托缓冲路径（本类不再回调 done）；
//   - CRC 已知 → open blob + fstat 校验 size + crc32Combine 组 19B 帧头 →
//     deliver 交给连接层 send(header)+sendFile(fd, offset, len)。
// 本类不 include net：连接层通过 deliver 注入；deliver 返回 true 后 fd 归连接关闭。
// 零拷贝路径不把 4MB 块读进用户态（懒填充是一次性例外）。
class SendfileBlockSender {
public:
    using Task = std::function<void()>;
    using Executor = std::function<bool(Task)>;  // IO 池
    using DoneCallback = std::function<void(cm::ErrorCode code)>;
    // 连接层注入：先 send(header19) 再 sendFile(fd, offset, len)。
    // 返回 true 表示已接管 fd（完成/失败/关闭时由连接关闭）；false 表示未受理，fd 由本类关闭。
    using Deliver = std::function<bool(const std::string& header, int fd,
                                       std::int64_t offset, std::size_t len)>;
    // 缓冲回退：懒填充失败时接管整个请求（委托 D-3 BlockSender）。
    // 在 IO 池线程调用；调用方负责把实际缓冲发送投回业务池（IO 池不碰 DB/Redis）。
    using Fallback = std::function<void(std::uint64_t uid, const std::string& ticket,
                                        std::uint64_t fid, std::uint32_t blockIndex,
                                        std::uint32_t seq)>;

    SendfileBlockSender(cm::db::MysqlPool& mysql,
                        cm::cache::RedisPool& redis,
                        Executor executor,
                        Deliver deliver,
                        Fallback fallback,
                        const std::string& dataDir);

    SendfileBlockSender(const SendfileBlockSender&) = delete;
    SendfileBlockSender& operator=(const SendfileBlockSender&) = delete;

    // done(Ok) = 已受理（连接接管 fd）；done(非 Ok) = 拒绝（1004/1001/3001/超时等）。
    // 懒填充失败走 fallback 时不再回调 done。
    void sendBlock(std::uint64_t uid,
                   const std::string& ticket,
                   std::uint64_t fid,
                   std::uint32_t blockIndex,
                   std::uint32_t seq,
                   DoneCallback done);

private:
    struct WorkItem {
        std::string blobPath;    // 绝对路径 dataDir/blobs/<f_path>
        std::uint64_t size;      // blob 总大小
        std::uint64_t offset;    // blockIndex * 4MB
        std::uint32_t len;       // min(4MB, size - offset)
        std::uint32_t blockIndex;
        std::uint32_t seq;
        std::uint64_t uid;
        std::string ticket;
        std::uint64_t fid;
        DoneCallback done;
    };

    void openAndDeliver(const WorkItem& item, std::uint32_t blockCrc);
    void fillAndDeliver(const WorkItem& item);

    cm::db::MysqlPool& m_mysql;
    DownloadTicket m_ticket;
    Executor m_executor;
    Deliver m_deliver;
    Fallback m_fallback;
    std::string m_dataDir;
};

}  // namespace logic
}  // namespace cm

#endif  // CM_DOWNLOAD_ZEROCOPY
