#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace cm {

// 流式 SHA-256（FIPS 180-4）。
// hex() 不修改当前对象：内部拷贝一份状态再收尾，因此在 hex() 之后仍可继续 update。
class Sha256 {
public:
    Sha256();

    void update(const void* data, std::size_t len);
    std::string hex() const;
    void reset();

private:
    void transform(const std::uint8_t* block);
    void finalize();

    std::uint32_t state_[8];
    std::uint64_t bitCount_;
    std::uint8_t buffer_[64];
    std::size_t bufferLen_;
    std::uint8_t digest_[32];
};

std::string sha256Hex(const void* data, std::size_t len);
std::string sha256Hex(const std::string& data);

}  // namespace cm
