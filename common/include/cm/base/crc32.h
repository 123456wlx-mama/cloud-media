#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace cm {

class Crc32 {
public:
    Crc32();
    void update(const void* data, std::size_t len);
    std::uint32_t value() const;

private:
    std::uint32_t crc_;
};

std::uint32_t crc32(const void* data, std::size_t len);
std::uint32_t crc32(const std::string& data);

// 计算 CRC(A || B)：crc1 = CRC(A)，crc2 = CRC(B)，len2 = B 的字节数。
// 输入与返回值都是已 finalize 的 CRC32（与 cm::crc32 一致）。
std::uint32_t crc32Combine(std::uint32_t crc1, std::uint32_t crc2,
                           std::uint64_t len2);

}  // namespace cm
