#pragma once

#include <cstdint>
#include <string>

#include "cm/base/error_code.h"
#include "cm/protocol/frame.h"

namespace cm {

class Buffer;

namespace protocol {

class Codec {
public:
    static ErrorCode encode(std::uint16_t type, std::uint32_t seq,
                            const std::string& payload, std::string* frame);
    static ErrorCode decode(Buffer& in, Frame* out, DecodeStatus* status);

    // 大帧专用：只解析帧头并把 payload 从 Buffer 移出，不校验 CRC。
    // 只接受 10006；其他类型返回 UnknownType 且不消费 Buffer。
    static ErrorCode decodeBulk(Buffer& in, RawFrame* out, DecodeStatus* status);

    // 业务线程校验：CRC 覆盖 header11 + payload，与 decode 的规则一致。
    static bool verifyFrameCrc(const RawFrame& frame);
};

}  // namespace protocol
}  // namespace cm
