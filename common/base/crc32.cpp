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

}  // namespace

Crc32::Crc32() : crc_(0xFFFFFFFFu) {}

void Crc32::update(const void* data, std::size_t len) {
    if (data == NULL || len == 0) {
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

}  // namespace cm
