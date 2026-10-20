#include "cm/base/crc32.h"

namespace cm {
namespace {

struct Crc32Table {
    std::uint32_t values[256];

    Crc32Table() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t crc = i;
            for (int bit = 0; bit < 8; ++bit) {
                if (crc & 1u) {
                    crc = 0xEDB88320u ^ (crc >> 1);
                } else {
                    crc >>= 1;
                }
            }
            values[i] = crc;
        }
    }
};

const Crc32Table& table() {
    static const Crc32Table instance;
    return instance;
}

std::uint32_t matrixTimes(const std::uint32_t* matrix, std::uint32_t vector) {
    std::uint32_t sum = 0;
    while (vector != 0) {
        if ((vector & 1u) != 0) {
            sum ^= *matrix;
        }
        vector >>= 1;
        ++matrix;
    }
    return sum;
}

void matrixSquare(std::uint32_t* square, const std::uint32_t* matrix) {
    for (std::size_t i = 0; i < 32; ++i) {
        square[i] = matrixTimes(matrix, matrix[i]);
    }
}

}  // namespace

Crc32::Crc32() : crc_(0xFFFFFFFFu) {}

void Crc32::update(const void* data, std::size_t len) {
    if (data == nullptr || len == 0) {
        return;
    }
    const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
    const Crc32Table& t = table();
    for (std::size_t i = 0; i < len; ++i) {
        crc_ = t.values[(crc_ ^ bytes[i]) & 0xFFu] ^ (crc_ >> 8);
    }
}

std::uint32_t Crc32::value() const {
    return crc_ ^ 0xFFFFFFFFu;
}

std::uint32_t crc32(const void* data, std::size_t len) {
    Crc32 crc;
    crc.update(data, len);
    return crc.value();
}

std::uint32_t crc32(const std::string& data) {
    return crc32(data.data(), data.size());
}

std::uint32_t crc32Combine(std::uint32_t crc1, std::uint32_t crc2,
                           std::uint64_t len2) {
    if (len2 == 0) {
        return crc1;
    }

    // 与 zlib 的 crc32_combine 等价：在 GF(2) 上用 32x32 矩阵平方，
    // 把「len2 个零比特」的移位作用到 crc1 上，再与 crc2 异或。
    std::uint32_t even[32];
    std::uint32_t odd[32];

    odd[0] = 0xEDB88320u;
    std::uint32_t row = 1;
    for (std::size_t i = 1; i < 32; ++i) {
        odd[i] = row;
        row <<= 1;
    }

    matrixSquare(even, odd);  // 2 个零比特
    matrixSquare(odd, even);  // 4 个零比特

    do {
        matrixSquare(even, odd);
        if ((len2 & 1u) != 0) {
            crc1 = matrixTimes(even, crc1);
        }
        len2 >>= 1;
        if (len2 == 0) {
            break;
        }

        matrixSquare(odd, even);
        if ((len2 & 1u) != 0) {
            crc1 = matrixTimes(odd, crc1);
        }
        len2 >>= 1;
    } while (len2 != 0);

    return crc1 ^ crc2;
}

}  // namespace cm
