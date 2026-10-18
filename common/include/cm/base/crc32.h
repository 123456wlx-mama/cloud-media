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

}  // namespace cm
