#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "cm/protocol/frame.h"
#include "logic/download_limits.h"
#include "logic/rate_limiter.h"

namespace cm {
namespace logic {

class DownloadService;
class BlockSender;
class SendfileBlockSender;

// 下载侧限速与流控（D-5 / D-6）。
//   - 连接级令牌桶 2MB/s（容量 4MB）、用户级 8MB/s（容量 16MB）
//   - 流数上限：全局 512、每用户 8（StreamLimiter）
// 令牌不足但等待 <= 2s 时返回 Timeout + waitMs，由 handler 用 conn.runAfter
// 延迟重发（不 sleep 工作线程）；等待 > 2s 才回 3006。
class DownloadRateLimit {
public:
    DownloadRateLimit();

    // 占一个流槽位；同一连接重复调用幂等。失败（超全局/每用户上限）→ 调用方回 3006。
    bool acquireStream(const std::string& connKey, std::uint64_t uid);
    void releaseStream(const std::string& connKey);

    // 预扣 bytes 令牌（按块计费）。
    //   Ok          —— 立即发；
    //   Timeout     —— *waitMs 后重发；
    //   RateLimited —— 回 3006。
    cm::ErrorCode consume(const std::string& connKey, std::uint64_t uid,
                          std::uint64_t bytes, std::uint64_t* waitMs);

    // 连接关闭：归还流槽位并回收该连接的桶。
    void onConnClose(const std::string& connKey);

    std::size_t activeStreams() const { return m_streams.activeStreams(); }

private:
    // 同一把锁保护桶表；桶本身无锁（每桶只在该锁内使用）。
    mutable std::mutex m_mutex;
    std::map<std::string, std::shared_ptr<TokenBucket> > m_connBuckets;
    std::map<std::uint64_t, std::shared_ptr<TokenBucket> > m_userBuckets;
    std::map<std::string, std::uint64_t> m_streamOwner;  // connKey -> uid
    StreamLimiter m_streams;
};

// 下载请求 handler（10010/10027/10013/10040）。不 include net：
// 发送回调由连接层注入，绑定到发起请求的连接。
class DownloadHandlers {
public:
    // 连接层注入的发送回调（每个请求对应一条连接）。
    struct Conn {
        // 编码并发送一个业务 RS（type, seq, payload）。
        std::function<void(std::uint16_t type, std::uint32_t seq,
                           const std::string& payload)> sendFrame;
        // 发送已编码的完整帧（缓冲路径的 10011：19B 头 + payload）。
        std::function<void(const std::string& frame)> sendRaw;
        // 发送 ERROR_RS（seq, code, message）。
        std::function<void(std::uint32_t seq, std::uint32_t code,
                           const std::string& message)> sendError;
        // sendfile 交付：先 send(header19) 再 sendFile(fd, offset, len)；返回是否受理。
        std::function<bool(const std::string& header, int fd,
                           std::int64_t offset, std::size_t len)> sendFile;
        // 连接标识（限速桶按连接维度）与延迟执行（令牌桶等待不阻塞工作线程）。
        std::string connKey;
        std::function<void(std::int64_t ms, std::function<void()> task)> runAfter;
    };

    // 把任务投回业务线程池；返回 false 表示池已拒绝（停机/队列满）。
    // 必须注入：限速重试的定时器回调在 IO 线程，不能在那里做 Redis/MySQL/文件 IO。
    using Poster = std::function<bool(std::function<void()>)>;

    // post 为空时退化为就地执行（仅供单测；生产路径必须注入）。
    DownloadHandlers(DownloadService& service, BlockSender& buffered,
                     Poster post = Poster());

    DownloadHandlers(const DownloadHandlers&) = delete;
    DownloadHandlers& operator=(const DownloadHandlers&) = delete;

    void handleOpen(std::uint64_t uid, const protocol::Frame& frame,
                    const Conn& conn);
    void handleStat(std::uint64_t uid, const protocol::Frame& frame,
                    const Conn& conn);
    void handleRange(std::uint64_t uid, const protocol::Frame& frame,
                     const Conn& conn);
    void handleDone(std::uint64_t uid, const protocol::Frame& frame,
                    const Conn& conn);

#if defined(CM_DOWNLOAD_ZEROCOPY) && (CM_DOWNLOAD_ZEROCOPY)
    void setSendfileSender(SendfileBlockSender& sender) { m_sendfile = &sender; }
#endif

    // 连接关闭时归还流槽位（由连接层调用）。
    void onConnClose(const std::string& connKey) { m_rate.onConnClose(connKey); }
    std::size_t activeStreams() const { return m_rate.activeStreams(); }

private:
    // 限速通过后真正取块（handleRange 的直接路径与 runAfter 延迟路径共用）。
    void sendRangeNow(std::uint64_t uid, const std::string& ticket,
                      std::uint64_t fId, std::uint32_t blockIndex,
                      std::uint32_t seq, const Conn& conn);

    DownloadService& m_service;
    BlockSender& m_buffered;
    Poster m_post;
    DownloadRateLimit m_rate;
#if defined(CM_DOWNLOAD_ZEROCOPY) && (CM_DOWNLOAD_ZEROCOPY)
    SendfileBlockSender* m_sendfile;
#endif
};

}  // namespace logic
}  // namespace cm
