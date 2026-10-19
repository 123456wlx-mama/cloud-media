#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "cm/base/error_code.h"

TEST(ErrorCodeTest, KnownCodesToString) {
    EXPECT_EQ(std::string("ERR_BAD_FRAME"),
              cm::to_string(cm::ProtocolError::BadFrame));
    EXPECT_EQ(std::string("ERR_VERSION"),
              cm::to_string(cm::ProtocolError::Version));
    EXPECT_EQ(std::string("ERR_UNKNOWN_TYPE"),
              cm::to_string(cm::ProtocolError::UnknownType));
    EXPECT_EQ(std::string("ERR_PAYLOAD_TOO_LARGE"),
              cm::to_string(cm::ProtocolError::PayloadTooLarge));
    EXPECT_EQ(std::string("ERR_CRC"), cm::to_string(cm::ProtocolError::Crc));
    EXPECT_EQ(std::string("ERR_HANDSHAKE_REQUIRED"),
              cm::to_string(cm::ProtocolError::HandshakeRequired));
    EXPECT_EQ(std::string("ERR_DEPRECATED"),
              cm::to_string(cm::ProtocolError::Deprecated));
    EXPECT_EQ(std::string("ERR_TIMEOUT"),
              cm::to_string(cm::ProtocolError::Timeout));
}

TEST(ErrorCodeTest, UnknownCodeFallsBack) {
    EXPECT_EQ(std::string("UnknownError"),
              cm::to_string(static_cast<cm::ErrorCode>(12345)));
    EXPECT_EQ(std::string("UnknownProtocolError"),
              cm::to_string(static_cast<cm::ProtocolError>(9999)));
    EXPECT_EQ(std::string("NeedMore"), cm::to_string(cm::ErrorCode::NeedMore));
}

TEST(ErrorCodeTest, RetryableClassification) {
    EXPECT_TRUE(cm::is_retryable(cm::ErrorCode::Timeout));
    EXPECT_TRUE(cm::is_retryable(cm::ErrorCode::NetworkError));
    EXPECT_TRUE(cm::is_retryable(cm::ErrorCode::ChunkMissing));
    EXPECT_TRUE(cm::is_retryable(cm::ErrorCode::Md5Mismatch));
    EXPECT_TRUE(cm::is_retryable(cm::ErrorCode::RateLimited));
    EXPECT_FALSE(cm::is_retryable(cm::ErrorCode::AuthFailed));
    EXPECT_FALSE(cm::is_retryable(cm::ErrorCode::InvalidArgument));

    EXPECT_TRUE(cm::is_retryable(cm::ProtocolError::Timeout));
    EXPECT_FALSE(cm::is_retryable(cm::ProtocolError::Crc));
}

TEST(ErrorCodeTest, Phase5BusinessCodes) {
    EXPECT_EQ(3007, static_cast<int>(cm::ErrorCode::TaskExpired));
    EXPECT_EQ(3008, static_cast<int>(cm::ErrorCode::TooManyNodes));

    EXPECT_EQ(std::string("TaskExpired"), cm::to_string(cm::ErrorCode::TaskExpired));
    EXPECT_EQ(std::string("TooManyNodes"), cm::to_string(cm::ErrorCode::TooManyNodes));

    // 3007 需要重新 init，3008 需要分批删除，都不是原样重试能解决的。
    EXPECT_FALSE(cm::is_retryable(cm::ErrorCode::TaskExpired));
    EXPECT_FALSE(cm::is_retryable(cm::ErrorCode::TooManyNodes));
}

TEST(ErrorCodeTest, ProtocolCodesAreStable) {
    EXPECT_EQ(9001u, static_cast<std::uint32_t>(cm::ProtocolError::BadFrame));
    EXPECT_EQ(9002u, static_cast<std::uint32_t>(cm::ProtocolError::Version));
    EXPECT_EQ(9003u, static_cast<std::uint32_t>(cm::ProtocolError::UnknownType));
    EXPECT_EQ(9004u, static_cast<std::uint32_t>(cm::ProtocolError::PayloadTooLarge));
    EXPECT_EQ(9005u, static_cast<std::uint32_t>(cm::ProtocolError::Crc));
    EXPECT_EQ(9006u, static_cast<std::uint32_t>(cm::ProtocolError::HandshakeRequired));
    EXPECT_EQ(9007u, static_cast<std::uint32_t>(cm::ProtocolError::Deprecated));
    EXPECT_EQ(9008u, static_cast<std::uint32_t>(cm::ProtocolError::Timeout));
}
