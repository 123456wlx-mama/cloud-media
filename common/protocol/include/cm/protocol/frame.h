#pragma once

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

}  // namespace protocol
}  // namespace cm
