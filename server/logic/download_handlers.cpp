#include "logic/download_handlers.h"

#include <cstdint>
#include <string>
#include <utility>

#include "cm/base/error_code.h"
#include "cm/protocol/message_type.h"
#include "cm/protocol/messages/download.h"
#include "logic/block_sender.h"
#include "logic/download_service.h"
#include "logic/sendfile_block_sender.h"

namespace cm {
namespace logic {

DownloadRateLimit::DownloadRateLimit() {}

bool DownloadRateLimit::acquireStream(const std::string& connKey,
                                      std::uint64_t uid) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<std::string, std::uint64_t>::const_iterator it =
        m_streamOwner.find(connKey);
    if (it != m_streamOwner.end()) {
        return true;  // 同连接重复 open：幂等，不重复占槽位
    }
    if (!m_streams.tryAcquire(uid)) {
        return false;
    }
    m_streamOwner[connKey] = uid;
    return true;
}

void DownloadRateLimit::releaseStream(const std::string& connKey) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<std::string, std::uint64_t>::iterator it = m_streamOwner.find(connKey);
    if (it == m_streamOwner.end()) {
        return;
    }
    m_streams.release(it->second);
    m_streamOwner.erase(it);
}

void DownloadRateLimit::onConnClose(const std::string& connKey) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<std::string, std::uint64_t>::iterator it = m_streamOwner.find(connKey);
    if (it != m_streamOwner.end()) {
        m_streams.release(it->second);
        m_streamOwner.erase(it);
    }
    m_connBuckets.erase(connKey);
}

cm::ErrorCode DownloadRateLimit::consume(const std::string& connKey,
                                         std::uint64_t uid,
                                         std::uint64_t bytes,
                                         std::uint64_t* waitMs) {
    if (waitMs != nullptr) {
        *waitMs = 0;
    }
    // 桶本身无锁，必须在同一把锁内消费；临界区只有几次浮点运算，不持锁做 IO。
    std::lock_guard<std::mutex> lock(m_mutex);
    std::shared_ptr<TokenBucket>& connBucket = m_connBuckets[connKey];
    if (!connBucket) {
        connBucket.reset(new TokenBucket(kConnTokenCapacity, kConnTokenRatePerSec));
    }
    std::shared_ptr<TokenBucket>& userBucket = m_userBuckets[uid];
    if (!userBucket) {
        userBucket.reset(new TokenBucket(kUserTokenCapacity, kUserTokenRatePerSec));
    }

    // 连接桶先扣；用户桶不足时连接桶已扣（只会更保守，不会超速）。
    std::uint64_t connWait = 0;
    const cm::ErrorCode connRc = connBucket->tryAcquire(bytes, &connWait);
    std::uint64_t userWait = 0;
    const cm::ErrorCode userRc = userBucket->tryAcquire(bytes, &userWait);

    if (connRc == cm::ErrorCode::RateLimited ||
        userRc == cm::ErrorCode::RateLimited) {
        return cm::ErrorCode::RateLimited;
    }
    if (connRc == cm::ErrorCode::Timeout || userRc == cm::ErrorCode::Timeout) {
        if (waitMs != nullptr) {
            *waitMs = connWait > userWait ? connWait : userWait;
        }
        return cm::ErrorCode::Timeout;
    }
    return cm::ErrorCode::Ok;
}

DownloadHandlers::DownloadHandlers(DownloadService& service,
                                   BlockSender& buffered)
    : m_service(service), m_buffered(buffered)
#if defined(CM_DOWNLOAD_ZEROCOPY) && (CM_DOWNLOAD_ZEROCOPY)
      ,
      m_sendfile(nullptr)
#endif
{
}

void DownloadHandlers::handleOpen(std::uint64_t uid,
                                  const protocol::Frame& frame,
                                  const Conn& conn) {
    protocol::DownloadOpenRq rq;
    if (!rq.decode(frame.payload)) {
        conn.sendError(frame.seq,
                       static_cast<std::uint32_t>(ErrorCode::InvalidArgument),
                       "bad open request");
        return;
    }

    // 流数上限：全局 512 / 每用户 8，超限回 3006（槽位在 Done 或连接关闭时归还）。
    if (rq.ufId != 0 || !rq.shareTicket.empty()) {
        if (!m_rate.acquireStream(conn.connKey, uid)) {
            conn.sendError(frame.seq, to_wire_code(ErrorCode::RateLimited),
                           "too many download streams");
            return;
        }
    }

    protocol::DownloadOpenRs rs;
    const ErrorCode rc = m_service.openDownload(uid, rq, &rs);
    if (rc != ErrorCode::Ok && rs.code == 0) {
        rs.code = to_wire_code(rc);
    }
    std::string payload;
    if (!rs.encode(&payload)) {
        return;
    }
    conn.sendFrame(static_cast<std::uint16_t>(
                       protocol::MessageType::DownloadOpenRs),
                   frame.seq, payload);
}

void DownloadHandlers::handleStat(std::uint64_t uid,
                                  const protocol::Frame& frame,
                                  const Conn& conn) {
    protocol::DownloadStatRq rq;
    if (!rq.decode(frame.payload)) {
        conn.sendError(frame.seq,
                       static_cast<std::uint32_t>(ErrorCode::InvalidArgument),
                       "bad stat request");
        return;
    }

    protocol::DownloadStatRs rs;
    const ErrorCode rc = m_service.stat(uid, rq, &rs);
    if (rc != ErrorCode::Ok && rs.code == 0) {
        rs.code = to_wire_code(rc);
    }
    std::string payload;
    if (!rs.encode(&payload)) {
        return;
    }
    conn.sendFrame(static_cast<std::uint16_t>(
                       protocol::MessageType::DownloadStatRs),
                   frame.seq, payload);
}

void DownloadHandlers::handleRange(std::uint64_t uid,
                                   const protocol::Frame& frame,
                                   const Conn& conn) {
    protocol::DownloadRangeRq rq;
    if (!rq.decode(frame.payload)) {
        conn.sendError(frame.seq,
                       static_cast<std::uint32_t>(ErrorCode::InvalidArgument),
                       "bad range request");
        return;
    }

    // 限速计费按 4MB 块；末块同样按 4MB 记（偏保守，实测约 1.7MB/s 而非 2MB/s）。
    const std::uint64_t kBillingBytes = 4194304;
    std::uint64_t waitMs = 0;
    const ErrorCode rl = m_rate.consume(conn.connKey, uid, kBillingBytes, &waitMs);
    if (rl == ErrorCode::RateLimited) {
        conn.sendError(frame.seq, to_wire_code(ErrorCode::RateLimited),
                       "rate limited");
        return;
    }
    if (rl == ErrorCode::Timeout && conn.runAfter) {
        // 令牌不足但在 2s 内可补足：交给 loop 定时器延迟重发，绝不 sleep 工作线程。
        const std::string ticket = rq.ticket;
        const std::uint64_t fId = rq.fId;
        const std::uint32_t blockIndex = rq.blockIndex;
        const std::uint32_t seq = frame.seq;
        const Conn delayed = conn;
        conn.runAfter(static_cast<std::int64_t>(waitMs),
                      [this, uid, ticket, fId, blockIndex, seq, delayed]() {
                          sendRangeNow(uid, ticket, fId, blockIndex, seq, delayed);
                      });
        return;
    }
    sendRangeNow(uid, rq.ticket, rq.fId, rq.blockIndex, frame.seq, conn);
}

void DownloadHandlers::sendRangeNow(std::uint64_t uid,
                                    const std::string& ticket,
                                    std::uint64_t fId,
                                    std::uint32_t blockIndex,
                                    std::uint32_t seq,
                                    const Conn& conn) {
    const std::uint32_t seq_ = seq;
    (void)seq_;

    // 缓冲路径的完成回调：Ok 发送原始 10011 帧，非 Ok 走 ERROR_RS。
    auto bufferedDone = [conn, seq](ErrorCode code, std::string fr) {
        if (code == ErrorCode::Ok) {
            conn.sendRaw(std::move(fr));
        } else {
            conn.sendError(seq, to_wire_code(code), "download range failed");
        }
    };

#if defined(CM_DOWNLOAD_ZEROCOPY) && (CM_DOWNLOAD_ZEROCOPY)
    if (m_sendfile != nullptr) {
        // sidecar 懒填充失败 → 回退缓冲路径（IO 池线程调用，回调已按值捕获）。
        auto fallback = [this, conn](std::uint64_t u, const std::string& tk,
                                     std::uint64_t f, std::uint32_t bi,
                                     std::uint32_t s) {
            m_buffered.sendBlock(u, tk, f, bi, s,
                                 [conn, s](ErrorCode code, std::string fr) {
                                     if (code == ErrorCode::Ok) {
                                         conn.sendRaw(std::move(fr));
                                     } else {
                                         conn.sendError(
                                             s, to_wire_code(code),
                                             "download range failed");
                                     }
                                 });
        };
        m_sendfile->sendBlock(
            uid, ticket, fId, blockIndex, seq, conn.sendFile,
            std::move(fallback),
            [conn, seq](ErrorCode code) {
                if (code != ErrorCode::Ok) {
                    conn.sendError(seq, to_wire_code(code),
                                   "download range failed");
                }
            });
        return;
    }
#endif

    m_buffered.sendBlock(uid, ticket, fId, blockIndex, seq,
                         std::move(bufferedDone));
}

void DownloadHandlers::handleDone(std::uint64_t uid,
                                  const protocol::Frame& frame,
                                  const Conn& conn) {
    protocol::DownloadDoneRq rq;
    if (!rq.decode(frame.payload)) {
        conn.sendError(frame.seq,
                       static_cast<std::uint32_t>(ErrorCode::InvalidArgument),
                       "bad done request");
        return;
    }

    protocol::DownloadDoneRs rs;
    const ErrorCode rc = m_service.done(uid, rq, &rs);
    if (rc != ErrorCode::Ok && rs.code == 0) {
        rs.code = to_wire_code(rc);
    }
    std::string payload;
    if (!rs.encode(&payload)) {
        return;
    }
    // 正常结束归还流槽位（异常断开由连接层 onConnClose 兜底）。
    m_rate.releaseStream(conn.connKey);
    conn.sendFrame(static_cast<std::uint16_t>(
                       protocol::MessageType::DownloadDoneRs),
                   frame.seq, payload);
}

}  // namespace logic
}  // namespace cm
