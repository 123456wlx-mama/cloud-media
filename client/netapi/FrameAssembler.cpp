#include "netapi/FrameAssembler.h"

#include "cm/protocol/codec.h"

namespace cm {
namespace netapi {

FrameAssembler::FrameAssembler() {}

bool FrameAssembler::feed(const char* data, std::size_t len,
                          std::vector<protocol::Frame>* out) {
    if (out == nullptr) {
        return false;
    }
    if (data != nullptr && len > 0 && !m_buffer.append(data, len)) {
        return false;  // 超过 Buffer 上限（16MB），视为异常
    }

    for (;;) {
        protocol::Frame frame;
        protocol::DecodeStatus status = protocol::DecodeStatus::NeedMore;
        const ErrorCode rc = protocol::Codec::decode(m_buffer, &frame, &status);
        if (rc == ErrorCode::Ok) {
            out->push_back(frame);
            continue;
        }
        if (rc == ErrorCode::NeedMore) {
            break;
        }
        // 坏帧：协议不可恢复，交给上层断开重连（与 server 的 ConnectionDecoder 一致）。
        return false;
    }

    // 半包时 Buffer 会随大帧增长（4MB 块），收完立刻收缩，避免常驻。
    if (m_buffer.readableBytes() == 0 && m_buffer.capacity() > 512 * 1024) {
        m_buffer.shrinkTo(64 * 1024);
    }
    return true;
}

void FrameAssembler::reset() { m_buffer.reset(); }

}  // namespace netapi
}  // namespace cm
