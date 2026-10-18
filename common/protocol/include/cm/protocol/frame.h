#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "cm/protocol/version.h"

namespace cm {
namespace protocol {

constexpr std::uint32_t kMagic = 0x434D4456u;
constexpr std::size_t kHeaderSize = 19;
constexpr std::uint32_t kMaxPayload = 8 * 1024 * 1024;

enum class DecodeStatus {
    Ok,
    NeedMore,
    BadMagic,
    BadVersion,
    BadLength,
    BadCrc,
    UnknownType
};

struct Frame {
    std::uint16_t type;
    std::uint32_t seq;
    std::string payload;

    Frame() : type(0), seq(0) {}
};

// 大帧（上传分片）专用：payload 拥有字节，不指向 Buffer。
// header11 是 VERSION..SEQ 共 11 字节，供业务线程校验 CRC。
struct RawFrame {
    std::uint16_t type;
    std::uint32_t seq;
    std::uint32_t len;
    std::uint32_t expectedCrc;
    std::array<std::uint8_t, 11> header11;
    std::string payload;

    RawFrame()
        : type(0), seq(0), len(0), expectedCrc(0) {
        header11.fill(0);
    }
};

}  // namespace protocol
}  // namespace cm
