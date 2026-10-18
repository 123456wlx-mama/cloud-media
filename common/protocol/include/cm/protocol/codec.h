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
};

}  // namespace protocol
}  // namespace cm
