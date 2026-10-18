#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "cm/base/buffer.h"
#include "cm/base/byte_order.h"
#include "cm/protocol/codec.h"
#include "cm/protocol/frame.h"
#include "cm/protocol/message_type.h"
#include "connection_decoder.h"

namespace {

std::string makeFrame(std::uint16_t type, std::uint32_t seq,
                      const std::string& payload) {
    std::string frame;
    const cm::ErrorCode code = cm::protocol::Codec::encode(type, seq, payload, &frame);
    EXPECT_EQ(cm::ErrorCode::Ok, code);
    return frame;
}

std::string makeBulkFrame(std::uint32_t seq, std::size_t payloadSize,
                          std::uint32_t fill) {
    const std::string payload(payloadSize, static_cast<char>(fill));
    return makeFrame(static_cast<std::uint16_t>(
                         cm::protocol::MessageType::UploadChunkRq),
                     seq, payload);
}

void corruptPayloadByte(std::string* frame) {
    ASSERT_NE(std::string::npos, frame->size());
    ASSERT_GE(frame->size(), cm::protocol::kHeaderSize + 1);
    (*frame)[cm::protocol::kHeaderSize] =
        static_cast<char>((*frame)[cm::protocol::kHeaderSize] ^ 0x01);
}

class GateHarness {
public:
    GateHarness()
        : frameCount(0),
          bulkCount(0),
          pauseCount(0),
          resumeCount(0),
          closeCount(0) {
        decoder = std::make_shared<cm::logic::ConnectionDecoder>(
            [this](const cm::protocol::Frame&) { ++frameCount; },
            [this](std::shared_ptr<cm::protocol::RawFrame> frame,
                   std::function<void(bool)> done) {
                ++bulkCount;
                lastBulk = frame;
                completion = done;
            },
            [this]() { ++pauseCount; },
            [this]() { ++resumeCount; },
            [this]() { ++closeCount; },
            [](std::function<void()> fn) { fn(); });
    }

    std::shared_ptr<cm::logic::ConnectionDecoder> decoder;
    std::function<void(bool)> completion;
    std::shared_ptr<cm::protocol::RawFrame> lastBulk;
    int frameCount;
    int bulkCount;
    int pauseCount;
    int resumeCount;
    int closeCount;
};

}  // namespace

TEST(BulkGateTest, CodecBulkParsesHeaderWithoutCrc) {
    const std::string payload(128, 'x');
    const std::string encoded = makeFrame(
        static_cast<std::uint16_t>(cm::protocol::MessageType::UploadChunkRq),
        7, payload);

    cm::Buffer in;
    ASSERT_TRUE(in.append(encoded));

    cm::protocol::RawFrame frame;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::NeedMore;
    EXPECT_EQ(cm::ErrorCode::Ok, cm::protocol::Codec::decodeBulk(in, &frame, &status));
    EXPECT_EQ(cm::protocol::DecodeStatus::Ok, status);
    EXPECT_EQ(std::uint16_t(10006), frame.type);
    EXPECT_EQ(std::uint32_t(7), frame.seq);
    EXPECT_EQ(std::uint32_t(payload.size()), frame.len);
    EXPECT_EQ(payload, frame.payload);
    EXPECT_TRUE(cm::protocol::Codec::verifyFrameCrc(frame));
}

TEST(BulkGateTest, CodecBulkBadMagicVersionLength) {
    const std::string encoded = makeBulkFrame(1, 32, 'a');

    {
        std::string bad = encoded;
        bad[0] = static_cast<char>(bad[0] ^ 0x01);
        cm::Buffer in;
        ASSERT_TRUE(in.append(bad));
        cm::protocol::RawFrame frame;
        cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::Ok;
        EXPECT_EQ(cm::ErrorCode::ProtocolError,
                  cm::protocol::Codec::decodeBulk(in, &frame, &status));
        EXPECT_EQ(cm::protocol::DecodeStatus::BadMagic, status);
        EXPECT_EQ(bad.size(), in.readableBytes());
    }

    {
        std::string bad = encoded;
        bad[4] = 3;
        cm::Buffer in;
        ASSERT_TRUE(in.append(bad));
        cm::protocol::RawFrame frame;
        cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::Ok;
        EXPECT_EQ(cm::ErrorCode::ProtocolError,
                  cm::protocol::Codec::decodeBulk(in, &frame, &status));
        EXPECT_EQ(cm::protocol::DecodeStatus::BadVersion, status);
    }

    {
        std::string bad = encoded;
        cm::writeU32BE(reinterpret_cast<std::uint8_t*>(&bad[7]),
                       static_cast<std::uint32_t>(cm::protocol::kMaxPayload + 1));
        cm::Buffer in;
        ASSERT_TRUE(in.append(bad));
        cm::protocol::RawFrame frame;
        cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::Ok;
        EXPECT_EQ(cm::ErrorCode::ProtocolError,
                  cm::protocol::Codec::decodeBulk(in, &frame, &status));
        EXPECT_EQ(cm::protocol::DecodeStatus::BadLength, status);
    }
}

TEST(BulkGateTest, CodecBulkRejectsNonBulkType) {
    const std::string encoded = makeFrame(
        static_cast<std::uint16_t>(cm::protocol::MessageType::UploadChunkRs),
        1, std::string("ok"));
    cm::Buffer in;
    ASSERT_TRUE(in.append(encoded));

    cm::protocol::RawFrame frame;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::Ok;
    EXPECT_EQ(cm::ErrorCode::ProtocolError,
              cm::protocol::Codec::decodeBulk(in, &frame, &status));
    EXPECT_EQ(cm::protocol::DecodeStatus::UnknownType, status);
    EXPECT_EQ(encoded.size(), in.readableBytes());
}

TEST(BulkGateTest, CodecBulkPayloadOwnedAfterBufferReset) {
    const std::string payload(4096, 'p');
    const std::string encoded = makeFrame(
        static_cast<std::uint16_t>(cm::protocol::MessageType::UploadChunkRq),
        2, payload);
    cm::Buffer in;
    ASSERT_TRUE(in.append(encoded));

    cm::protocol::RawFrame frame;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::Ok;
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::protocol::Codec::decodeBulk(in, &frame, &status));
    ASSERT_EQ(payload, frame.payload);

    in.reset();
    ASSERT_TRUE(in.append(std::string(128, 'z')));
    EXPECT_EQ(payload, frame.payload);
    EXPECT_LE(in.capacity(), std::size_t(64 * 1024));
}

TEST(BulkGateTest, BufferShrinkPreservesPipelinedFrame) {
    const std::string bulk = makeBulkFrame(3, 4 * 1024 * 1024, 'b');
    const std::string next = makeFrame(
        static_cast<std::uint16_t>(cm::protocol::MessageType::UploadChunkRs),
        4, std::string("next"));
    cm::Buffer in;
    ASSERT_TRUE(in.append(bulk));
    ASSERT_TRUE(in.append(next));

    cm::protocol::RawFrame frame;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::Ok;
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::protocol::Codec::decodeBulk(in, &frame, &status));
    ASSERT_EQ(next.size(), in.readableBytes());
    EXPECT_EQ(next, std::string(in.peek(), in.readableBytes()));
    EXPECT_LE(in.capacity(), std::size_t(64 * 1024));
}

TEST(BulkGateTest, BufferCapacityDropsAfterBulk) {
    const std::string bulk = makeBulkFrame(5, 4 * 1024 * 1024, 'c');
    cm::Buffer in;
    ASSERT_TRUE(in.append(bulk));
    EXPECT_GT(in.capacity(), std::size_t(4 * 1024 * 1024));

    cm::protocol::RawFrame frame;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::Ok;
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::protocol::Codec::decodeBulk(in, &frame, &status));
    EXPECT_LE(in.capacity(), std::size_t(64 * 1024));
}

TEST(BulkGateTest, BulkGateSecondFrameNotDecodedUntilVerified) {
    const std::string bulk = makeBulkFrame(6, 1024, 'd');
    const std::string next = makeFrame(
        static_cast<std::uint16_t>(cm::protocol::MessageType::UploadChunkRs),
        7, std::string("after"));

    GateHarness harness;
    cm::Buffer in;
    ASSERT_TRUE(in.append(bulk));
    ASSERT_TRUE(in.append(next));

    EXPECT_EQ(cm::logic::ConnectionDecoder::Result::BulkDispatched,
              harness.decoder->onData(in));
    EXPECT_EQ(1, harness.bulkCount);
    EXPECT_EQ(0, harness.frameCount);
    EXPECT_EQ(1, harness.pauseCount);
    EXPECT_TRUE(harness.decoder->bulkPending());
    ASSERT_TRUE(harness.completion);

    harness.completion(true);
    EXPECT_EQ(0, harness.frameCount);
    EXPECT_EQ(1, harness.resumeCount);
    EXPECT_FALSE(harness.decoder->bulkPending());

    EXPECT_EQ(cm::logic::ConnectionDecoder::Result::NeedMore,
              harness.decoder->onData(in));
    EXPECT_EQ(1, harness.frameCount);
}

TEST(BulkGateTest, BulkGateBadCrcClosesAndDropsNext) {
    std::string bulk = makeBulkFrame(8, 1024, 'e');
    corruptPayloadByte(&bulk);
    const std::string next = makeFrame(
        static_cast<std::uint16_t>(cm::protocol::MessageType::UploadChunkRs),
        9, std::string("dropped"));

    GateHarness harness;
    cm::Buffer in;
    ASSERT_TRUE(in.append(bulk));
    ASSERT_TRUE(in.append(next));

    EXPECT_EQ(cm::logic::ConnectionDecoder::Result::BulkDispatched,
              harness.decoder->onData(in));
    ASSERT_TRUE(harness.completion);
    harness.completion(false);

    EXPECT_EQ(1, harness.closeCount);
    EXPECT_EQ(0, harness.frameCount);
    EXPECT_EQ(cm::logic::ConnectionDecoder::Result::Error,
              harness.decoder->onData(in));
    EXPECT_EQ(0, harness.frameCount);
}

TEST(BulkGateTest, BulkNoLongTerm4MbBuffer) {
    GateHarness harness;
    for (int i = 0; i < 10; ++i) {
        const std::string bulk = makeBulkFrame(
            static_cast<std::uint32_t>(100 + i), 4 * 1024 * 1024,
            static_cast<char>('a' + i));
        cm::Buffer in;
        ASSERT_TRUE(in.append(bulk));
        EXPECT_EQ(cm::logic::ConnectionDecoder::Result::BulkDispatched,
                  harness.decoder->onData(in));
        ASSERT_TRUE(harness.completion);
        harness.completion(true);
        EXPECT_LE(in.capacity(), std::size_t(64 * 1024)) << "iteration=" << i;
    }
}

TEST(BulkGateTest, NormalDecodeStillVerifiesCrc) {
    std::string bad = makeFrame(
        static_cast<std::uint16_t>(cm::protocol::MessageType::UploadChunkRs),
        10, std::string("payload"));
    corruptPayloadByte(&bad);
    cm::Buffer in;
    ASSERT_TRUE(in.append(bad));

    cm::protocol::Frame frame;
    cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::Ok;
    EXPECT_EQ(cm::ErrorCode::ProtocolError,
              cm::protocol::Codec::decode(in, &frame, &status));
    EXPECT_EQ(cm::protocol::DecodeStatus::BadCrc, status);
}
