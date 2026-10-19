#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "cm/protocol/frame.h"

namespace cm {
namespace logic {

class DownloadService;
class BlockSender;
class SendfileBlockSender;

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
    };

    DownloadHandlers(DownloadService& service, BlockSender& buffered);

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

private:
    DownloadService& m_service;
    BlockSender& m_buffered;
#if defined(CM_DOWNLOAD_ZEROCOPY) && (CM_DOWNLOAD_ZEROCOPY)
    SendfileBlockSender* m_sendfile;
#endif
};

}  // namespace logic
}  // namespace cm
