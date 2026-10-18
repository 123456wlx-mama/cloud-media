#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace cm {
namespace protocol {

const std::size_t kMaxStringBytes = 64 * 1024;
const std::size_t kMaxSerializeBytes = 8 * 1024 * 1024;

class Writer {
public:
    Writer();

    void clear();
    bool writeU8(std::uint8_t value);
    bool writeU16(std::uint16_t value);
    bool writeU32(std::uint32_t value);
    bool writeU64(std::uint64_t value);
    bool writeString(const std::string& value);
    bool writeBytes(const void* data, std::size_t len);

    const std::string& data() const;

private:
    std::string data_;
};

class Reader {
public:
    explicit Reader(const std::string& data);

    bool readU8(std::uint8_t* value);
    bool readU16(std::uint16_t* value);
    bool readU32(std::uint32_t* value);
    bool readU64(std::uint64_t* value);
    bool readString(std::string* value);
    bool readBytes(std::string* value, std::size_t len);

    bool ok() const;
    std::size_t remaining() const;

private:
    bool need(std::size_t len);

    const std::string& data_;
    std::size_t offset_;
    bool ok_;
};

}  // namespace protocol
}  // namespace cm
