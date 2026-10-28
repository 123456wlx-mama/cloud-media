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

namespace {

std::string MakeBulkHeader(std::uint16_t type, std::uint32_t len,
                           std::uint32_t seq) {
    std::string header(cm::protocol::kHeaderSize, '\0');
    std::uint8_t* p = reinterpret_cast<std::uint8_t*>(&header[0]);
    cm::writeU32BE(p, cm::protocol::kMagic);
    p[4] = static_cast<std::uint8_t>(cm::protocol::kVersion);
    cm::writeU16BE(p + 5, type);
    cm::writeU32BE(p + 7, len);
    cm::writeU32BE(p + 11, seq);
    cm::writeU32BE(p + 15, 0);
    return header;
}

}  // namespace

TEST(CodecBulkTest, NeedsFullHeader) {
    cm::Buffer buffer;
    ASSERT_TRUE(buffer.append(MakeBulkHeader(10006, 0, 1).substr(0, 18)));
    cm::protocol::RawFrame raw;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::Ok;
    EXPECT_EQ(cm::ErrorCode::NeedMore,
              cm::protocol::Codec::decodeBulk(buffer, &raw, &status));
    EXPECT_EQ(cm::protocol::DecodeStatus::NeedMore, status);
    EXPECT_EQ(std::size_t(18), buffer.readableBytes());
}

TEST(CodecBulkTest, EmptyPayloadAccepted) {
    cm::Buffer buffer;
    ASSERT_TRUE(buffer.append(MakeBulkHeader(10006, 0, 7)));
    cm::protocol::RawFrame raw;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::Ok;
    EXPECT_EQ(cm::ErrorCode::Ok,
              cm::protocol::Codec::decodeBulk(buffer, &raw, &status));
    EXPECT_EQ(cm::protocol::DecodeStatus::Ok, status);
    EXPECT_EQ(static_cast<std::uint16_t>(10006), raw.type);
    EXPECT_EQ(std::uint32_t(7), raw.seq);
    EXPECT_EQ(std::uint32_t(0), raw.len);
    EXPECT_TRUE(raw.payload.empty());
    EXPECT_EQ(std::size_t(0), buffer.readableBytes());
}

TEST(CodecBulkTest, PayloadAndSeqExtracted) {
    cm::Buffer buffer;
    ASSERT_TRUE(buffer.append(MakeBulkHeader(10006, 6, 9) + "abcdef"));
    cm::protocol::RawFrame raw;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::Ok;
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::protocol::Codec::decodeBulk(buffer, &raw, &status));
    EXPECT_EQ(std::uint32_t(6), raw.len);
    EXPECT_EQ(std::string("abcdef"), raw.payload);
    EXPECT_EQ(std::size_t(11), raw.header11.size());
}

TEST(CodecBulkTest, WaitsForWholePayload) {
    cm::Buffer buffer;
    ASSERT_TRUE(buffer.append(MakeBulkHeader(10006, 6, 9) + "abc"));
    cm::protocol::RawFrame raw;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::Ok;
    EXPECT_EQ(cm::ErrorCode::NeedMore,
              cm::protocol::Codec::decodeBulk(buffer, &raw, &status));
    EXPECT_EQ(std::size_t(22), buffer.readableBytes());
}

TEST(CodecBulkTest, RejectsNonBulkTypeWithoutConsuming) {
    cm::Buffer buffer;
    const std::string frame = Encode(10002, 1, "x");
    ASSERT_TRUE(buffer.append(frame));
    cm::protocol::RawFrame raw;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::Ok;
    EXPECT_EQ(cm::ErrorCode::ProtocolError,
              cm::protocol::Codec::decodeBulk(buffer, &raw, &status));
    EXPECT_EQ(cm::protocol::DecodeStatus::UnknownType, status);
    EXPECT_EQ(frame.size(), buffer.readableBytes());
}

TEST(CodecBulkTest, RejectsBadMagicAndVersion) {
    cm::Buffer buffer;
    std::string badMagic = MakeBulkHeader(10006, 0, 1);
    badMagic[0] = 0;
    ASSERT_TRUE(buffer.append(badMagic));
    cm::protocol::RawFrame raw;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::Ok;
    EXPECT_EQ(cm::ErrorCode::ProtocolError,
              cm::protocol::Codec::decodeBulk(buffer, &raw, &status));
    EXPECT_EQ(cm::protocol::DecodeStatus::BadMagic, status);

    buffer.reset();
    std::string badVersion = MakeBulkHeader(10006, 0, 1);
    badVersion[4] = static_cast<char>(0xEE);
    ASSERT_TRUE(buffer.append(badVersion));
    EXPECT_EQ(cm::ErrorCode::ProtocolError,
              cm::protocol::Codec::decodeBulk(buffer, &raw, &status));
    EXPECT_EQ(cm::protocol::DecodeStatus::BadVersion, status);
}

TEST(CodecBulkTest, RejectsOversizeLength) {
    cm::Buffer buffer;
    ASSERT_TRUE(buffer.append(
        MakeBulkHeader(10006, cm::protocol::kMaxPayload + 1, 1)));
    cm::protocol::RawFrame raw;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::Ok;
    EXPECT_EQ(cm::ErrorCode::ProtocolError,
              cm::protocol::Codec::decodeBulk(buffer, &raw, &status));
    EXPECT_EQ(cm::protocol::DecodeStatus::BadLength, status);
}

// 回归：普通 decode 取出大 payload 后必须立刻缩容，
// 否则连接会长期滞留十几 MB，并让 readFd 的溢出路径逼近上限。
TEST(CodecTest, ReleasesReadBufferAfterLargeFrame) {
    const std::string payload(1024 * 1024, 'x');
    const std::string encoded = Encode(10002, 1, payload);

    cm::Buffer buffer;
    AppendAll(buffer, encoded);
    ASSERT_GT(buffer.capacity(), 512 * 1024);

    cm::protocol::Frame frame;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::NeedMore;
    ASSERT_EQ(cm::ErrorCode::Ok, cm::protocol::Codec::decode(buffer, &frame, &status));
    ASSERT_EQ(payload.size(), frame.payload.size());
    EXPECT_EQ(0u, buffer.readableBytes());
    EXPECT_LE(buffer.capacity(), 64 * 1024);
}
