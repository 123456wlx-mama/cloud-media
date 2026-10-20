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

// 业务层 ErrorCode → wire code（U-3b 统一映射）。
// 只覆盖与枚举值不同的两处：Ok→0、TaskNotFound→3002（按 U-3b 指令）；
// 其余 1xxx/2xxx/3xxx/4xxx 与枚举值一致，直接沿用。
// CacheStore 的 Busy 由服务层转成 Timeout(1006)，不在本函数出现。
std::uint32_t to_wire_code(ErrorCode code);

// wire code → ErrorCode（与 to_wire_code 互逆；0→Ok，未知→UnknownError）。
// 注意 3002 歧义：to_wire_code 把 TaskNotFound 折成 ChunkMissing，反向只能得到
// ChunkMissing。
ErrorCode wire_code_to_error(std::uint32_t wireCode);

// 解码失败 → wire code：BadCrc 回 9005（02 §2.2 的“回 ERROR_RS 且不带 payload”），
// 其余（BadMagic/BadVersion/BadLength/UnknownType）统一回 1001。
// 放在 common 而不是 net，避免 connection_decoder（logic）反向依赖 net。
std::uint32_t decode_failure_wire_code(bool crcMismatch);

const char* to_string(ProtocolError code);
bool is_retryable(ProtocolError code);

}  // namespace cm
