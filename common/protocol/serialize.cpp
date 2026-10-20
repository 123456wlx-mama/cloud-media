#include "cm/protocol/serialize.h"

#include "cm/base/byte_order.h"

namespace cm {
namespace protocol {

Writer::Writer() : data_() {}

void Writer::clear() {
    data_.clear();
}

bool Writer::writeBytes(const void* data, std::size_t len) {
    if (data == nullptr && len != 0) {
        return false;
    }
    if (data_.size() > kMaxSerializeBytes ||
        len > kMaxSerializeBytes - data_.size()) {
        return false;
    }
    if (len != 0) {
        data_.append(static_cast<const char*>(data), len);
    }
    return true;
}

bool Writer::writeU8(std::uint8_t value) {
    if (data_.size() >= kMaxSerializeBytes) {
        return false;
    }
    data_.push_back(static_cast<char>(value));
    return true;
}

bool Writer::writeU16(std::uint16_t value) {
    std::uint8_t bytes[2];
    writeU16BE(bytes, value);
    return writeBytes(bytes, sizeof(bytes));
}

bool Writer::writeU32(std::uint32_t value) {
    std::uint8_t bytes[4];
    writeU32BE(bytes, value);
    return writeBytes(bytes, sizeof(bytes));
}

bool Writer::writeU64(std::uint64_t value) {
    std::uint8_t bytes[8];
    writeU64BE(bytes, value);
    return writeBytes(bytes, sizeof(bytes));
}

bool Writer::writeString(const std::string& value) {
    if (value.size() > kMaxStringBytes) {
        return false;
    }
    if (!writeU32(static_cast<std::uint32_t>(value.size()))) {
        return false;
    }
    return writeBytes(value.data(), value.size());
}

const std::string& Writer::data() const {
    return data_;
}

Reader::Reader(const std::string& data)
    : data_(data), offset_(0), ok_(true) {}

bool Reader::ok() const {
    return ok_;
}

std::size_t Reader::remaining() const {
    return data_.size() - offset_;
}

bool Reader::need(std::size_t len) {
    if (!ok_) {
        return false;
    }
    if (len > remaining()) {
        ok_ = false;
        return false;
    }
    return true;
}

bool Reader::readU8(std::uint8_t* value) {
    if (value == nullptr || !need(1)) {
        return false;
    }
    *value = static_cast<std::uint8_t>(data_[offset_]);
    ++offset_;
    return true;
}

bool Reader::readU16(std::uint16_t* value) {
    if (value == nullptr || !need(2)) {
        return false;
    }
    *value = readU16BE(reinterpret_cast<const std::uint8_t*>(data_.data() + offset_));
    offset_ += 2;
    return true;
}

bool Reader::readU32(std::uint32_t* value) {
    if (value == nullptr || !need(4)) {
        return false;
    }
    *value = readU32BE(reinterpret_cast<const std::uint8_t*>(data_.data() + offset_));
    offset_ += 4;
    return true;
}

bool Reader::readU64(std::uint64_t* value) {
    if (value == nullptr || !need(8)) {
        return false;
    }
    *value = readU64BE(reinterpret_cast<const std::uint8_t*>(data_.data() + offset_));
    offset_ += 8;
    return true;
}

bool Reader::readBytes(std::string* value, std::size_t len) {
    if (value == nullptr || !need(len)) {
        return false;
    }
    value->assign(data_.data() + offset_, len);
    offset_ += len;
    return true;
}

bool Reader::readString(std::string* value) {
    if (value == nullptr) {
        return false;
    }
    std::uint32_t len = 0;
    if (!readU32(&len)) {
        return false;
    }
    if (len > kMaxStringBytes || len > remaining()) {
        ok_ = false;
        return false;
    }
    return readBytes(value, len);
}

}  // namespace protocol
}  // namespace cm
