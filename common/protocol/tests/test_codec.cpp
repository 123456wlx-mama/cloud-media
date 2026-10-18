#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "cm/base/buffer.h"
#include "cm/base/byte_order.h"
#include "cm/base/error_code.h"
#include "cm/protocol/codec.h"
#include "cm/protocol/frame.h"

namespace {

std::string Encode(std::uint16_t type, std::uint32_t seq,
                   const std::string& payload) {
    std::string frame;
    EXPECT_TRUE(cm::protocol::Codec::encode(type, seq, payload, &frame) ==
                cm::ErrorCode::Ok);
    return frame;
}

void AppendAll(cm::Buffer& buffer, const std::string& data) {
    ASSERT_TRUE(buffer.append(data));
}

}  // namespace

TEST(CodecTest, EncodeDecodeNormal) {
    std::string encoded = Encode(10002, 7, "hello");
    ASSERT_FALSE(encoded.empty());

    cm::Buffer buffer;
    AppendAll(buffer, encoded);

    cm::protocol::Frame frame;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::NeedMore;
    cm::ErrorCode code = cm::protocol::Codec::decode(buffer, &frame, &status);

    EXPECT_TRUE(code == cm::ErrorCode::Ok);
    EXPECT_TRUE(status == cm::protocol::DecodeStatus::Ok);
    EXPECT_EQ(static_cast<std::uint16_t>(10002), frame.type);
    EXPECT_EQ(static_cast<std::uint32_t>(7), frame.seq);
    EXPECT_EQ(std::string("hello"), frame.payload);
    EXPECT_EQ(static_cast<std::size_t>(0), buffer.readableBytes());
}

TEST(CodecTest, HalfPacketNeedsMore) {
    std::string encoded = Encode(10002, 7, "hello");
    ASSERT_GE(encoded.size(), static_cast<std::size_t>(10));

    cm::Buffer buffer;
    ASSERT_TRUE(buffer.append(encoded.data(), 10));

    cm::protocol::Frame frame;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::Ok;
    cm::ErrorCode code = cm::protocol::Codec::decode(buffer, &frame, &status);

    EXPECT_TRUE(code == cm::ErrorCode::NeedMore);
    EXPECT_TRUE(status == cm::protocol::DecodeStatus::NeedMore);
    EXPECT_EQ(static_cast<std::size_t>(10), buffer.readableBytes());

    ASSERT_TRUE(buffer.append(encoded.data() + 10, encoded.size() - 10));
    code = cm::protocol::Codec::decode(buffer, &frame, &status);
    EXPECT_TRUE(code == cm::ErrorCode::Ok);
    EXPECT_TRUE(status == cm::protocol::DecodeStatus::Ok);
    EXPECT_EQ(std::string("hello"), frame.payload);
}

TEST(CodecTest, StickyPacketsDecodeSequentially) {
    std::string first = Encode(10002, 1, "one");
    std::string second = Encode(10003, 2, "two");

    cm::Buffer buffer;
    AppendAll(buffer, first);
    AppendAll(buffer, second);

    cm::protocol::Frame frame;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::NeedMore;
    EXPECT_TRUE(cm::protocol::Codec::decode(buffer, &frame, &status) ==
                cm::ErrorCode::Ok);
    EXPECT_EQ(static_cast<std::uint32_t>(1), frame.seq);
    EXPECT_EQ(std::string("one"), frame.payload);

    EXPECT_TRUE(cm::protocol::Codec::decode(buffer, &frame, &status) ==
                cm::ErrorCode::Ok);
    EXPECT_EQ(static_cast<std::uint32_t>(2), frame.seq);
    EXPECT_EQ(std::string("two"), frame.payload);
    EXPECT_EQ(static_cast<std::size_t>(0), buffer.readableBytes());
}

TEST(CodecTest, BadCrcRejected) {
    std::string encoded = Encode(10002, 7, "hello");
    ASSERT_FALSE(encoded.empty());
    encoded[encoded.size() - 1] ^= 0x01;

    cm::Buffer buffer;
    AppendAll(buffer, encoded);

    cm::protocol::Frame frame;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::Ok;
    EXPECT_TRUE(cm::protocol::Codec::decode(buffer, &frame, &status) ==
                cm::ErrorCode::ProtocolError);
    EXPECT_TRUE(status == cm::protocol::DecodeStatus::BadCrc);
}

TEST(CodecTest, BadVersionRejected) {
    std::string encoded = Encode(10002, 7, "hello");
    ASSERT_GE(encoded.size(), static_cast<std::size_t>(5));
    encoded[4] = 1;

    cm::Buffer buffer;
    AppendAll(buffer, encoded);

    cm::protocol::Frame frame;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::Ok;
    EXPECT_TRUE(cm::protocol::Codec::decode(buffer, &frame, &status) ==
                cm::ErrorCode::ProtocolError);
    EXPECT_TRUE(status == cm::protocol::DecodeStatus::BadVersion);
}

TEST(CodecTest, BadLengthRejected) {
    std::string encoded = Encode(10002, 7, "hello");
    ASSERT_GE(encoded.size(), static_cast<std::size_t>(11));
    cm::writeU32BE(
        reinterpret_cast<std::uint8_t*>(&encoded[7]),
        cm::protocol::kMaxPayload + 1);

    cm::Buffer buffer;
    AppendAll(buffer, encoded);

    cm::protocol::Frame frame;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::Ok;
    EXPECT_TRUE(cm::protocol::Codec::decode(buffer, &frame, &status) ==
                cm::ErrorCode::ProtocolError);
    EXPECT_TRUE(status == cm::protocol::DecodeStatus::BadLength);
}

TEST(CodecTest, UnknownTypeRejected) {
    std::string encoded = Encode(19999, 7, "x");
    ASSERT_FALSE(encoded.empty());

    cm::Buffer buffer;
    AppendAll(buffer, encoded);

    cm::protocol::Frame frame;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::Ok;
    EXPECT_TRUE(cm::protocol::Codec::decode(buffer, &frame, &status) ==
                cm::ErrorCode::ProtocolError);
    EXPECT_TRUE(status == cm::protocol::DecodeStatus::UnknownType);
}

TEST(CodecTest, EmptyPayloadAccepted) {
    std::string encoded = Encode(11003, 42, "");
    ASSERT_FALSE(encoded.empty());

    cm::Buffer buffer;
    AppendAll(buffer, encoded);

    cm::protocol::Frame frame;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::NeedMore;
    cm::ErrorCode code = cm::protocol::Codec::decode(buffer, &frame, &status);

    EXPECT_TRUE(code == cm::ErrorCode::Ok);
    EXPECT_TRUE(status == cm::protocol::DecodeStatus::Ok);
    EXPECT_EQ(static_cast<std::uint16_t>(11003), frame.type);
    EXPECT_EQ(static_cast<std::uint32_t>(42), frame.seq);
    EXPECT_TRUE(frame.payload.empty());
}
