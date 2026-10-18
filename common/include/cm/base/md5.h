#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace cm {

// 流式 MD5（RFC 1321）。hex() 不修改当前对象，可在计算摘要后继续 update。
class Md5 {
public:
    Md5();

    void update(const void* data, std::size_t len);
    std::string hex() const;
    void reset();

private:
    void transform(const std::uint8_t* block);
    void finalize();

    std::uint32_t state_[4];
    std::uint64_t bitCount_;
    std::uint8_t buffer_[64];
    std::size_t bufferLen_;
};

std::string md5Hex(const void* data, std::size_t len);
std::string md5Hex(const std::string& data);

}  // namespace cm
