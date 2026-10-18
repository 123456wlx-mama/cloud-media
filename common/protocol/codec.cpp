#include "cm/protocol/codec.h"

#include <cstddef>

#include "cm/base/buffer.h"
#include "cm/base/byte_order.h"
#include "cm/base/crc32.h"
#include "cm/protocol/message_type.h"

namespace cm {
namespace protocol {

ErrorCode Codec::encode(std::uint16_t type, std::uint32_t seq,
                        const std::string& payload, std::string* frame) {
    if (frame == NULL || payload.size() > kMaxPayload) {
        return ErrorCode::InvalidArgument;
    }

    std::string encoded(kHeaderSize, '\0');
    std::uint8_t* header = reinterpret_cast<std::uint8_t*>(&encoded[0]);
    writeU32BE(header, kMagic);
    header[4] = static_cast<std::uint8_t>(kVersion);
    writeU16BE(header + 5, type);
    writeU32BE(header + 7, static_cast<std::uint32_t>(payload.size()));
    writeU32BE(header + 11, seq);

    encoded.append(payload);

    // CRC 覆盖 version(1)+type(2)+len(4)+seq(4) 共 11 字节 + payload，
    // 跳过 CRC 字段本身（offset 15~18），否则解码侧永远校验失败。
    // 注意：必须在 append 之后重新取 data()，header 指针可能因扩容而失效。
    cm::Crc32 checksum;
    checksum.update(reinterpret_cast<const std::uint8_t*>(encoded.data()) + 4, 11);
    if (!payload.empty()) {
        checksum.update(payload.data(), payload.size());
    }
    writeU32BE(reinterpret_cast<std::uint8_t*>(&encoded[15]), checksum.value());
    *frame = encoded;
    return ErrorCode::Ok;
}

ErrorCode Codec::decode(Buffer& in, Frame* out, DecodeStatus* status) {
    if (out == NULL || status == NULL) {
        return ErrorCode::InvalidArgument;
    }
    if (in.readableBytes() < kHeaderSize) {
        *status = DecodeStatus::NeedMore;
        return ErrorCode::NeedMore;
    }

    const std::uint8_t* header = reinterpret_cast<const std::uint8_t*>(in.peek());
    const std::uint32_t magic = readU32BE(header);
    const std::uint8_t version = header[4];
    const std::uint16_t type = readU16BE(header + 5);
    const std::uint32_t len = readU32BE(header + 7);
    const std::uint32_t seq = readU32BE(header + 11);
    const std::uint32_t expectedCrc = readU32BE(header + 15);

    if (magic != kMagic) {
        *status = DecodeStatus::BadMagic;
        return ErrorCode::ProtocolError;
    }
    if (version != static_cast<std::uint8_t>(kVersion)) {
        *status = DecodeStatus::BadVersion;
        return ErrorCode::ProtocolError;
    }
    if (len > kMaxPayload) {
        *status = DecodeStatus::BadLength;
        return ErrorCode::ProtocolError;
    }
    if (in.readableBytes() < kHeaderSize + static_cast<std::size_t>(len)) {
        *status = DecodeStatus::NeedMore;
        return ErrorCode::NeedMore;
    }

    // 与 encode 一致：CRC 覆盖 version/type/len/seq + payload，跳过 CRC 字段本身。
    cm::Crc32 checksum;
    checksum.update(header + 4, 11);
    if (len > 0) {
        checksum.update(header + kHeaderSize, static_cast<std::size_t>(len));
    }
    if (checksum.value() != expectedCrc) {
        *status = DecodeStatus::BadCrc;
        return ErrorCode::ProtocolError;
    }
    if (!isKnownType(type)) {
        *status = DecodeStatus::UnknownType;
        return ErrorCode::ProtocolError;
    }

    if (!in.retrieve(kHeaderSize)) {
        *status = DecodeStatus::NeedMore;
        return ErrorCode::NeedMore;
    }
    out->type = type;
    out->seq = seq;
    out->payload = in.retrieveAsString(len);
    *status = DecodeStatus::Ok;
    return ErrorCode::Ok;
}

}  // namespace protocol
}  // namespace cm
