#include "cm/base/error_code.h"

namespace cm {

const char* to_string(ErrorCode code) {
    switch (code) {
        case ErrorCode::Ok: return "Ok";
        case ErrorCode::InvalidArgument: return "InvalidArgument";
        case ErrorCode::NotFound: return "NotFound";
        case ErrorCode::AlreadyExists: return "AlreadyExists";
        case ErrorCode::PermissionDenied: return "PermissionDenied";
        case ErrorCode::NoSpace: return "NoSpace";
        case ErrorCode::Timeout: return "Timeout";
        case ErrorCode::NetworkError: return "NetworkError";
        case ErrorCode::InternalError: return "InternalError";
        case ErrorCode::ProtocolError: return "ProtocolError";
        case ErrorCode::NeedMore: return "NeedMore";
        case ErrorCode::UnknownError: return "UnknownError";
        case ErrorCode::AuthFailed: return "AuthFailed";
        case ErrorCode::TokenExpired: return "TokenExpired";
        case ErrorCode::ShareExpired: return "ShareExpired";
        case ErrorCode::AccessCodeWrong: return "AccessCodeWrong";
        case ErrorCode::FileGone: return "FileGone";
        case ErrorCode::ChunkMissing: return "ChunkMissing";
        case ErrorCode::Md5Mismatch: return "Md5Mismatch";
        case ErrorCode::TaskNotFound: return "TaskNotFound";
        case ErrorCode::TaskStateInvalid: return "TaskStateInvalid";
        case ErrorCode::RateLimited: return "RateLimited";
        case ErrorCode::TaskExpired: return "TaskExpired";
        case ErrorCode::TooManyNodes: return "TooManyNodes";
        case ErrorCode::DbUnavailable: return "DbUnavailable";
        case ErrorCode::DbPoolTimeout: return "DbPoolTimeout";
        case ErrorCode::DbQueryFailed: return "DbQueryFailed";
        case ErrorCode::DbTxConflict: return "DbTxConflict";
        case ErrorCode::RedisUnavailable: return "RedisUnavailable";
        case ErrorCode::CacheRebuildFailed: return "CacheRebuildFailed";
        case ErrorCode::AuthUnavailable: return "AuthUnavailable";
        case ErrorCode::RateLimitUnavailable: return "RateLimitUnavailable";
    }
    return "UnknownError";
}

std::uint32_t to_wire_code(ErrorCode code) {
    if (code == ErrorCode::Ok) {
        return 0;
    }
    // 06 §4.2 的 TaskNotFound 是 3004；U-3b 统一映射表要求回 3002，
    // 这里按指令覆盖。服务层若已显式写入 rs->code，则以服务层为准。
    if (code == ErrorCode::TaskNotFound) {
        return static_cast<std::uint32_t>(ErrorCode::ChunkMissing);
    }
    return static_cast<std::uint32_t>(code);
}

std::uint32_t decode_failure_wire_code(bool crcMismatch) {
    if (crcMismatch) {
        return static_cast<std::uint32_t>(ProtocolError::Crc);
    }
    return static_cast<std::uint32_t>(ErrorCode::InvalidArgument);
}

bool is_retryable(ErrorCode code) {
    switch (code) {
        case ErrorCode::Timeout:
        case ErrorCode::NetworkError:
        case ErrorCode::ChunkMissing:
        case ErrorCode::Md5Mismatch:
        case ErrorCode::RateLimited:
        case ErrorCode::DbUnavailable:
        case ErrorCode::DbPoolTimeout:
        case ErrorCode::DbTxConflict:
        case ErrorCode::RedisUnavailable:
        case ErrorCode::AuthUnavailable:
        case ErrorCode::RateLimitUnavailable:
        case ErrorCode::NeedMore:
            return true;
        case ErrorCode::TaskExpired:
        case ErrorCode::TooManyNodes:
            return false;  // 3007 需重新 init；3008 需分批删除，重试无意义
        default:
            return false;
    }
}

const char* to_string(ProtocolError code) {
    switch (code) {
        case ProtocolError::BadFrame: return "ERR_BAD_FRAME";
        case ProtocolError::Version: return "ERR_VERSION";
        case ProtocolError::UnknownType: return "ERR_UNKNOWN_TYPE";
        case ProtocolError::PayloadTooLarge: return "ERR_PAYLOAD_TOO_LARGE";
        case ProtocolError::Crc: return "ERR_CRC";
        case ProtocolError::HandshakeRequired: return "ERR_HANDSHAKE_REQUIRED";
        case ProtocolError::Deprecated: return "ERR_DEPRECATED";
        case ProtocolError::Timeout: return "ERR_TIMEOUT";
    }
    return "UnknownProtocolError";
}

bool is_retryable(ProtocolError code) {
    return code == ProtocolError::Timeout;
}

}  // namespace cm
