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

namespace {

struct ErrorRow {
    cm::ErrorCode code;
    const char* name;
    bool retryable;
    std::uint32_t wire;
};

const ErrorRow kErrors[] = {
    {cm::ErrorCode::Ok, "Ok", false, 0},
    {cm::ErrorCode::InvalidArgument, "InvalidArgument", false, 1001},
    {cm::ErrorCode::NotFound, "NotFound", false, 1002},
    {cm::ErrorCode::AlreadyExists, "AlreadyExists", false, 1003},
    {cm::ErrorCode::PermissionDenied, "PermissionDenied", false, 1004},
    {cm::ErrorCode::NoSpace, "NoSpace", false, 1005},
    {cm::ErrorCode::Timeout, "Timeout", true, 1006},
    {cm::ErrorCode::NetworkError, "NetworkError", true, 1007},
    {cm::ErrorCode::InternalError, "InternalError", false, 1008},
    {cm::ErrorCode::ProtocolError, "ProtocolError", false, 1009},
    {cm::ErrorCode::NeedMore, "NeedMore", true, 1010},
    {cm::ErrorCode::UnknownError, "UnknownError", false, 1099},
    {cm::ErrorCode::AuthFailed, "AuthFailed", false, 2001},
    {cm::ErrorCode::TokenExpired, "TokenExpired", false, 2002},
    {cm::ErrorCode::ShareExpired, "ShareExpired", false, 2003},
    {cm::ErrorCode::AccessCodeWrong, "AccessCodeWrong", false, 2004},
    {cm::ErrorCode::FileGone, "FileGone", false, 3001},
    {cm::ErrorCode::ChunkMissing, "ChunkMissing", true, 3002},
    {cm::ErrorCode::Md5Mismatch, "Md5Mismatch", true, 3003},
    {cm::ErrorCode::TaskNotFound, "TaskNotFound", false, 3002},
    {cm::ErrorCode::TaskStateInvalid, "TaskStateInvalid", false, 3005},
    {cm::ErrorCode::RateLimited, "RateLimited", true, 3006},
    {cm::ErrorCode::TaskExpired, "TaskExpired", false, 3007},
    {cm::ErrorCode::TooManyNodes, "TooManyNodes", false, 3008},
    {cm::ErrorCode::DbUnavailable, "DbUnavailable", true, 4001},
    {cm::ErrorCode::DbPoolTimeout, "DbPoolTimeout", true, 4002},
    {cm::ErrorCode::DbQueryFailed, "DbQueryFailed", false, 4003},
    {cm::ErrorCode::DbTxConflict, "DbTxConflict", true, 4004},
    {cm::ErrorCode::RedisUnavailable, "RedisUnavailable", true, 4005},
    {cm::ErrorCode::CacheRebuildFailed, "CacheRebuildFailed", false, 4006},
    {cm::ErrorCode::AuthUnavailable, "AuthUnavailable", true, 4007},
    {cm::ErrorCode::RateLimitUnavailable, "RateLimitUnavailable", true, 4008},
};

}  // namespace

TEST(ErrorCodeTest, FullToStringAndRetryableTable) {
    for (std::size_t i = 0; i < sizeof(kErrors) / sizeof(kErrors[0]); ++i) {
        EXPECT_EQ(std::string(kErrors[i].name), cm::to_string(kErrors[i].code))
            << "index=" << i;
        EXPECT_EQ(kErrors[i].retryable, cm::is_retryable(kErrors[i].code))
            << "code=" << kErrors[i].name;
    }
}

TEST(ErrorCodeTest, ToWireCodeAndBack) {
    for (std::size_t i = 0; i < sizeof(kErrors) / sizeof(kErrors[0]); ++i) {
        const ErrorRow& row = kErrors[i];
        EXPECT_EQ(row.wire, cm::to_wire_code(row.code)) << row.name;
        if (row.code == cm::ErrorCode::TaskNotFound) {
            // 3002 与 ChunkMissing 冲突，反向只能还原成 ChunkMissing。
            EXPECT_EQ(cm::ErrorCode::ChunkMissing,
                      cm::wire_code_to_error(row.wire));
            continue;
        }
        EXPECT_EQ(row.code, cm::wire_code_to_error(row.wire)) << row.name;
    }
}

TEST(ErrorCodeTest, UnknownWireCodeFallsBack) {
    EXPECT_EQ(cm::ErrorCode::UnknownError, cm::wire_code_to_error(9999));
    EXPECT_EQ(cm::ErrorCode::UnknownError, cm::wire_code_to_error(9001));
    EXPECT_EQ(cm::ErrorCode::UnknownError, cm::wire_code_to_error(4294967295U));
    EXPECT_EQ(cm::ErrorCode::Ok, cm::wire_code_to_error(0));
}
