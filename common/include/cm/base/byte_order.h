#pragma once

#include <cstdint>

namespace cm {

inline bool isLittleEndian() {
    const std::uint16_t value = 1;
    return *reinterpret_cast<const std::uint8_t*>(&value) == 1;
}

inline void writeU16BE(std::uint8_t* out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    out[1] = static_cast<std::uint8_t>(value & 0xFF);
}

inline std::uint16_t readU16BE(const std::uint8_t* in) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(in[0]) << 8) |
        static_cast<std::uint16_t>(in[1]));
}

inline void writeU32BE(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    out[1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    out[2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    out[3] = static_cast<std::uint8_t>(value & 0xFF);
}

inline std::uint32_t readU32BE(const std::uint8_t* in) {
    return (static_cast<std::uint32_t>(in[0]) << 24) |
           (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) |
           static_cast<std::uint32_t>(in[3]);
}

inline void writeU64BE(std::uint8_t* out, std::uint64_t value) {
    out[0] = static_cast<std::uint8_t>((value >> 56) & 0xFF);
    out[1] = static_cast<std::uint8_t>((value >> 48) & 0xFF);
    out[2] = static_cast<std::uint8_t>((value >> 40) & 0xFF);
    out[3] = static_cast<std::uint8_t>((value >> 32) & 0xFF);
    out[4] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    out[5] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    out[6] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    out[7] = static_cast<std::uint8_t>(value & 0xFF);
}

inline std::uint64_t readU64BE(const std::uint8_t* in) {
    return (static_cast<std::uint64_t>(in[0]) << 56) |
           (static_cast<std::uint64_t>(in[1]) << 48) |
           (static_cast<std::uint64_t>(in[2]) << 40) |
           (static_cast<std::uint64_t>(in[3]) << 32) |
           (static_cast<std::uint64_t>(in[4]) << 24) |
           (static_cast<std::uint64_t>(in[5]) << 16) |
           (static_cast<std::uint64_t>(in[6]) << 8) |
           static_cast<std::uint64_t>(in[7]);
}

inline std::uint16_t hostToBE16(std::uint16_t value) {
    if (!isLittleEndian()) {
        return value;
    }
    return static_cast<std::uint16_t>((value >> 8) | (value << 8));
}

inline std::uint32_t hostToBE32(std::uint32_t value) {
    if (!isLittleEndian()) {
        return value;
    }
    return ((value & 0x000000FFu) << 24) |
           ((value & 0x0000FF00u) << 8) |
           ((value & 0x00FF0000u) >> 8) |
           ((value & 0xFF000000u) >> 24);
}

inline std::uint64_t hostToBE64(std::uint64_t value) {
    if (!isLittleEndian()) {
        return value;
    }
    return ((value & 0x00000000000000FFull) << 56) |
           ((value & 0x000000000000FF00ull) << 40) |
           ((value & 0x0000000000FF0000ull) << 24) |
           ((value & 0x00000000FF000000ull) << 8) |
           ((value & 0x000000FF00000000ull) >> 8) |
           ((value & 0x0000FF0000000000ull) >> 24) |
           ((value & 0x00FF000000000000ull) >> 40) |
           ((value & 0xFF00000000000000ull) >> 56);
}

inline std::uint16_t beToHost16(std::uint16_t value) { return hostToBE16(value); }
inline std::uint32_t beToHost32(std::uint32_t value) { return hostToBE32(value); }
inline std::uint64_t beToHost64(std::uint64_t value) { return hostToBE64(value); }

}  // namespace cm
