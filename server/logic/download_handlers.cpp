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
    const std::uint32_t seq = frame.seq;

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
            uid, rq.ticket, rq.fId, rq.blockIndex, seq, conn.sendFile,
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

    m_buffered.sendBlock(uid, rq.ticket, rq.fId, rq.blockIndex, seq,
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
    conn.sendFrame(static_cast<std::uint16_t>(
                       protocol::MessageType::DownloadDoneRs),
                   frame.seq, payload);
}

}  // namespace logic
}  // namespace cm
