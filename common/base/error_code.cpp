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
    }
    return "UnknownError";
}

bool is_retryable(ErrorCode code) {
    switch (code) {
        case ErrorCode::Timeout:
        case ErrorCode::NetworkError:
        case ErrorCode::ChunkMissing:
        case ErrorCode::Md5Mismatch:
        case ErrorCode::RateLimited:
        case ErrorCode::NeedMore:
            return true;
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
