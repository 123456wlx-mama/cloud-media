#include "cm/base/md5.h"

#include <cstring>

namespace cm {
namespace {

const std::uint32_t kK[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
};

const unsigned char kS[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

std::uint32_t rotateLeft(std::uint32_t value, unsigned int bits) {
    return (value << bits) | (value >> (32 - bits));
}

}  // namespace

Md5::Md5() {
    reset();
}

void Md5::reset() {
    state_[0] = 0x67452301u;
    state_[1] = 0xefcdab89u;
    state_[2] = 0x98badcfeu;
    state_[3] = 0x10325476u;
    bitCount_ = 0;
    bufferLen_ = 0;
    std::memset(buffer_, 0, sizeof buffer_);
}

void Md5::update(const void* data, std::size_t len) {
    if (data == NULL || len == 0) {
        return;
    }
    const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
    bitCount_ += static_cast<std::uint64_t>(len) * 8u;

    while (len > 0) {
        const std::size_t space = sizeof buffer_ - bufferLen_;
        const std::size_t take = len < space ? len : space;
        std::memcpy(buffer_ + bufferLen_, bytes, take);
        bufferLen_ += take;
        bytes += take;
        len -= take;
        if (bufferLen_ == sizeof buffer_) {
            transform(buffer_);
            bufferLen_ = 0;
        }
    }
}

void Md5::transform(const std::uint8_t* block) {
    std::uint32_t m[16];
    for (std::size_t i = 0; i < 16; ++i) {
        m[i] = static_cast<std::uint32_t>(block[i * 4]) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24);
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];

    for (std::uint32_t i = 0; i < 64; ++i) {
        std::uint32_t f = 0;
        std::uint32_t g = 0;
        if (i < 16) {
            f = (b & c) | ((~b) & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | ((~d) & c);
            g = (5u * i + 1u) % 16u;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3u * i + 5u) % 16u;
        } else {
            f = c ^ (b | (~d));
            g = (7u * i) % 16u;
        }

        const std::uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rotateLeft(a + f + kK[i] + m[g], kS[i]);
        a = tmp;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
}

void Md5::finalize() {
    const std::uint64_t bits = bitCount_;
    std::uint8_t padding[72];
    std::memset(padding, 0, sizeof padding);
    padding[0] = 0x80;
    const std::size_t paddingLen =
        bufferLen_ < 56 ? 56 - bufferLen_ : 120 - bufferLen_;

    update(padding, paddingLen);

    std::uint8_t lengthBytes[8];
    for (std::size_t i = 0; i < 8; ++i) {
        lengthBytes[i] = static_cast<std::uint8_t>((bits >> (i * 8)) & 0xffu);
    }
    update(lengthBytes, sizeof lengthBytes);
}

std::string Md5::hex() const {
    Md5 copy(*this);
    copy.finalize();

    static const char kHex[] = "0123456789abcdef";
    char out[33];
    for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            const std::uint8_t byte =
                static_cast<std::uint8_t>((copy.state_[i] >> (j * 8)) & 0xffu);
            out[i * 8 + j * 2] = kHex[(byte >> 4) & 0x0fu];
            out[i * 8 + j * 2 + 1] = kHex[byte & 0x0fu];
        }
    }
    return std::string(out, 32);
}

std::string md5Hex(const void* data, std::size_t len) {
    Md5 md5;
    md5.update(data, len);
    return md5.hex();
}

std::string md5Hex(const std::string& data) {
    return md5Hex(data.data(), data.size());
}

}  // namespace cm
