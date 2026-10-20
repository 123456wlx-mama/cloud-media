#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "cm/base/error_code.h"
#include "cm/protocol/frame.h"
#include "cm/protocol/message_type.h"
#include "handler_registry.h"

namespace {

using cm::protocol::DecodeStatus;
using cm::protocol::Frame;
using cm::protocol::MessageType;

struct ErrorRecord {
    bool called;
    std::uint32_t seq;
    std::uint32_t wireCode;
    std::string message;

    ErrorRecord() : called(false), seq(0), wireCode(0) {}
};

net::HandlerRegistry::ErrorResponder makeResponder(ErrorRecord* record) {
    return [record](const net::ConnectionPtr&, std::uint32_t seq,
                    std::uint32_t wireCode, const std::string& message) {
        record->called = true;
        record->seq = seq;
        record->wireCode = wireCode;
        record->message = message;
    };
}

Frame makeFrame(std::uint16_t type, std::uint32_t seq) {
    Frame frame;
    frame.type = type;
    frame.seq = seq;
    frame.payload = std::string("payload");
    return frame;
}

}  // namespace

// 注册后能分发；重复注册返回 InvalidArgument。
TEST(HandlerRegistryTest, RegistersAndDispatches) {
    ErrorRecord error;
    // workers=NULL：同步执行，仅用于单测。
    net::HandlerRegistry registry(nullptr, makeResponder(&error));

    int calls = 0;
    std::uint32_t seenSeq = 0;
    EXPECT_EQ(cm::ErrorCode::Ok,
              registry.registerHandler(
                  static_cast<std::uint16_t>(MessageType::UploadInitRq),
                  [&calls, &seenSeq](const net::ConnectionPtr&, const Frame& frame) {
                      ++calls;
                      seenSeq = frame.seq;
                  }));
    EXPECT_TRUE(registry.hasHandler(
        static_cast<std::uint16_t>(MessageType::UploadInitRq)));
    EXPECT_EQ(std::size_t(1), registry.size());

    const Frame frame = makeFrame(
        static_cast<std::uint16_t>(MessageType::UploadInitRq), 41);
    EXPECT_EQ(cm::ErrorCode::Ok,
              registry.dispatch(frame.type, net::ConnectionPtr(), frame));
    EXPECT_EQ(1, calls);
    EXPECT_EQ(std::uint32_t(41), seenSeq);
    EXPECT_FALSE(error.called);
}

TEST(HandlerRegistryTest, RejectsDuplicateRegistration) {
    ErrorRecord error;
    net::HandlerRegistry registry(nullptr, makeResponder(&error));

    const std::uint16_t type = static_cast<std::uint16_t>(MessageType::UploadChunkRq);
    EXPECT_EQ(cm::ErrorCode::Ok,
              registry.registerHandler(type,
                                      [](const net::ConnectionPtr&, const Frame&) {}));
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              registry.registerHandler(type,
                                      [](const net::ConnectionPtr&, const Frame&) {}));
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              registry.registerHandler(0,
                                      [](const net::ConnectionPtr&, const Frame&) {}));
    EXPECT_EQ(cm::ErrorCode::InvalidArgument,
              registry.registerHandler(
                  static_cast<std::uint16_t>(MessageType::UploadInitRq),
                  net::HandlerRegistry::Handler()));
}

// 未注册类型回 9003，且不执行任何 handler。
TEST(HandlerRegistryTest, RejectsUnknownTypeWith9003) {
    ErrorRecord error;
    net::HandlerRegistry registry(nullptr, makeResponder(&error));

    const Frame frame = makeFrame(
        static_cast<std::uint16_t>(MessageType::ListFileRq), 9);
    EXPECT_EQ(cm::ErrorCode::ProtocolError,
              registry.dispatch(frame.type, net::ConnectionPtr(), frame));
    EXPECT_TRUE(error.called);
    EXPECT_EQ(std::uint32_t(9003), error.wireCode);
    EXPECT_EQ(std::uint32_t(9), error.seq);
}

// 解码失败统一 1001；BadCrc 保持 9005（02 §2.2 硬规则）。
TEST(HandlerRegistryTest, MapsDecodeFailuresTo1001) {
    EXPECT_EQ(std::uint32_t(1001), net::HandlerRegistry::decodeFailureCode());
    EXPECT_EQ(std::uint32_t(1001),
              net::HandlerRegistry::wireCodeForDecodeStatus(DecodeStatus::BadMagic));
    EXPECT_EQ(std::uint32_t(1001),
              net::HandlerRegistry::wireCodeForDecodeStatus(DecodeStatus::BadVersion));
    EXPECT_EQ(std::uint32_t(1001),
              net::HandlerRegistry::wireCodeForDecodeStatus(DecodeStatus::BadLength));
    EXPECT_EQ(std::uint32_t(1001),
              net::HandlerRegistry::wireCodeForDecodeStatus(DecodeStatus::UnknownType));
    EXPECT_EQ(std::uint32_t(9005),
              net::HandlerRegistry::wireCodeForDecodeStatus(DecodeStatus::BadCrc));
    EXPECT_EQ(std::uint32_t(0),
              net::HandlerRegistry::wireCodeForDecodeStatus(DecodeStatus::NeedMore));
}

// tryBegin 收进分发路径：准入失败不调 handler，回 1006；成功才执行并释放。
TEST(HandlerRegistryTest, AppliesAdmissionBeforeHandler) {
    ErrorRecord error;
    net::HandlerRegistry registry(nullptr, makeResponder(&error));

    int handlerCalls = 0;
    int releaseCalls = 0;
    const std::uint16_t busyType =
        static_cast<std::uint16_t>(MessageType::UploadChunkRq);
    EXPECT_EQ(cm::ErrorCode::Ok,
              registry.registerHandler(
                  busyType,
                  [&handlerCalls](const net::ConnectionPtr&, const Frame&) {
                      ++handlerCalls;
                  },
                  [](const net::ConnectionPtr&) { return false; },
                  [&releaseCalls](const net::ConnectionPtr&) { ++releaseCalls; }));

    const Frame busy = makeFrame(busyType, 11);
    EXPECT_EQ(cm::ErrorCode::Timeout,
              registry.dispatch(busyType, net::ConnectionPtr(), busy));
    EXPECT_EQ(0, handlerCalls);
    EXPECT_EQ(0, releaseCalls);
    EXPECT_TRUE(error.called);
    EXPECT_EQ(std::uint32_t(1006), error.wireCode);

    int admittedCalls = 0;
    const std::uint16_t okType =
        static_cast<std::uint16_t>(MessageType::UploadInitRq);
    EXPECT_EQ(cm::ErrorCode::Ok,
              registry.registerHandler(
                  okType,
                  [&admittedCalls](const net::ConnectionPtr&, const Frame&) {
                      ++admittedCalls;
                  },
                  [](const net::ConnectionPtr&) { return true; },
                  [&releaseCalls](const net::ConnectionPtr&) { ++releaseCalls; }));
    const Frame admitted = makeFrame(okType, 12);
    EXPECT_EQ(cm::ErrorCode::Ok,
              registry.dispatch(okType, net::ConnectionPtr(), admitted));
    EXPECT_EQ(1, admittedCalls);
    EXPECT_EQ(1, releaseCalls);
}

// 类型表必须与 U-1 的 message_type.h 一致：10000~10043 与 11000~11005。
TEST(HandlerRegistryTest, TypeTableMatchesU1) {
    for (std::uint16_t type = 10000; type <= 10043; ++type) {
        EXPECT_TRUE(cm::protocol::isKnownType(type)) << "type=" << type;
    }
    for (std::uint16_t type = 11000; type <= 11005; ++type) {
        EXPECT_TRUE(cm::protocol::isKnownType(type)) << "type=" << type;
    }
    EXPECT_FALSE(cm::protocol::isKnownType(10044));
    EXPECT_FALSE(cm::protocol::isKnownType(10999));

    EXPECT_EQ(10004, static_cast<int>(MessageType::UploadInitRq));
    EXPECT_EQ(10005, static_cast<int>(MessageType::UploadInitRs));
    EXPECT_EQ(10006, static_cast<int>(MessageType::UploadChunkRq));
    EXPECT_EQ(10011, static_cast<int>(MessageType::DownloadDataRs));
    EXPECT_EQ(10024, static_cast<int>(MessageType::QuickUploadRs));
    EXPECT_EQ(10030, static_cast<int>(MessageType::UploadCompleteRq));
    EXPECT_EQ(10043, static_cast<int>(MessageType::ShareCancelRs));
}

// 统一错误映射表（U-3b）。
TEST(HandlerRegistryTest, MapsBusinessErrorCodes) {
    EXPECT_EQ(std::uint32_t(0), cm::to_wire_code(cm::ErrorCode::Ok));
    EXPECT_EQ(std::uint32_t(1001), cm::to_wire_code(cm::ErrorCode::InvalidArgument));
    EXPECT_EQ(std::uint32_t(1002), cm::to_wire_code(cm::ErrorCode::NotFound));
    EXPECT_EQ(std::uint32_t(1003), cm::to_wire_code(cm::ErrorCode::AlreadyExists));
    EXPECT_EQ(std::uint32_t(1006), cm::to_wire_code(cm::ErrorCode::Timeout));
    EXPECT_EQ(std::uint32_t(3002), cm::to_wire_code(cm::ErrorCode::TaskNotFound));
    EXPECT_EQ(std::uint32_t(3002), cm::to_wire_code(cm::ErrorCode::ChunkMissing));
    EXPECT_EQ(std::uint32_t(3003), cm::to_wire_code(cm::ErrorCode::Md5Mismatch));
    EXPECT_EQ(std::uint32_t(3007), cm::to_wire_code(cm::ErrorCode::TaskExpired));
    EXPECT_EQ(std::uint32_t(3008), cm::to_wire_code(cm::ErrorCode::TooManyNodes));
    EXPECT_EQ(std::uint32_t(4001), cm::to_wire_code(cm::ErrorCode::DbUnavailable));
    EXPECT_EQ(std::uint32_t(4002), cm::to_wire_code(cm::ErrorCode::DbPoolTimeout));
    EXPECT_EQ(std::uint32_t(4005), cm::to_wire_code(cm::ErrorCode::RedisUnavailable));
    EXPECT_EQ(std::uint32_t(4007), cm::to_wire_code(cm::ErrorCode::AuthUnavailable));
}
