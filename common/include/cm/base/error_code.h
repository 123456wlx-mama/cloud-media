#pragma once

#include <cstdint>

namespace cm {

enum class ErrorCode {
    Ok = 0,
    InvalidArgument = 1001,
    NotFound = 1002,
    AlreadyExists = 1003,
    PermissionDenied = 1004,
    NoSpace = 1005,
    Timeout = 1006,
    NetworkError = 1007,
    InternalError = 1008,
    ProtocolError = 1009,
    NeedMore = 1010,
    UnknownError = 1099,
    AuthFailed = 2001,
    TokenExpired = 2002,
    ShareExpired = 2003,
    AccessCodeWrong = 2004,
    FileGone = 3001,
    ChunkMissing = 3002,
    Md5Mismatch = 3003,
    TaskNotFound = 3004,
    TaskStateInvalid = 3005,
    RateLimited = 3006,
    TaskExpired = 3007,   // 上传任务已过期，需重新初始化
    TooManyNodes = 3008,  // 彻底删除的子树节点数超过 10000

    // 存储层错误码（Phase 4）
    DbUnavailable = 4001,
    DbPoolTimeout = 4002,
    DbQueryFailed = 4003,
    DbTxConflict = 4004,
    RedisUnavailable = 4005,
    CacheRebuildFailed = 4006,
    AuthUnavailable = 4007,
    RateLimitUnavailable = 4008
};

// 协议错误码：ERROR_RS 的 wire code，数值与 docs/specs/03 第 13.2 节一致。
enum class ProtocolError : std::uint32_t {
    BadFrame = 9001,
    Version = 9002,
    UnknownType = 9003,
    PayloadTooLarge = 9004,
    Crc = 9005,
    HandshakeRequired = 9006,
    Deprecated = 9007,
    Timeout = 9008
};

const char* to_string(ErrorCode code);
bool is_retryable(ErrorCode code);

const char* to_string(ProtocolError code);
bool is_retryable(ProtocolError code);

}  // namespace cm
