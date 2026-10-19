#pragma once

namespace cm {
namespace logic {

// 块发送实现选择（D-4）。CM_DOWNLOAD_ZEROCOPY=1 时选 sendfile 零拷贝，
// 否则选 D-3 的缓冲实现（BlockSender）。
enum class BlockSenderKind {
    Buffered,   // D-3 缓冲实现（读 4MB + 流式 CRC + send）
    Sendfile    // D-4 sendfile 零拷贝实现（帧头 + sendfile）
};

class BlockSenderFactory {
public:
    // 编译期选择；D-6 与测试据此决定用哪种实现。
    static BlockSenderKind select();
};

}  // namespace logic
}  // namespace cm
