#pragma once

#include <cstdint>

namespace cm {
namespace protocol {

enum class MessageType : std::uint16_t {
    RegisterRq = 10000,
    RegisterRs = 10001,
    LoginRq = 10002,
    LoginRs = 10003,
    UploadFileRq = 10004,
    UploadFileRs = 10005,
    FileContentRq = 10006,
    FileContentRs = 10007,
    GetFileInfoRq = 10008,
    GetFileInfoRs = 10009,
    DownloadFileRq = 10010,
    DownloadFolderRq = 10011,
    DownloadFileRs = 10012,
    FileHeaderRq = 10013,
    FileHeaderRs = 10014,
    AddFolderRq = 10015,
    AddFolderRs = 10016,
    QuickUploadRs = 10017,
    ShareFileRq = 10018,
    ShareFileRs = 10019,
    MyShareRq = 10020,
    MyShareRs = 10021,
    GetShareRq = 10022,
    GetShareRs = 10023,
    FolderHeaderRq = 10024,
    DeleteFileRq = 10025,
    DeleteFileRs = 10026,
    ContinueDownloadRq = 10027,
    ContinueUploadRq = 10028,
    ContinueUploadRs = 10029,

    HelloReq = 11000,
    HelloRs = 11001,
    ErrorRs = 11002,
    HeartbeatReq = 11003,
    HeartbeatRs = 11004,
    LogoutReq = 11005
};

inline bool isKnownType(std::uint16_t type) {
    switch (type) {
        case 10000: case 10001: case 10002: case 10003: case 10004:
        case 10005: case 10006: case 10007: case 10008: case 10009:
        case 10010: case 10011: case 10012: case 10013: case 10014:
        case 10015: case 10016: case 10017: case 10018: case 10019:
        case 10020: case 10021: case 10022: case 10023: case 10024:
        case 10025: case 10026: case 10027: case 10028: case 10029:
        case 11000: case 11001: case 11002: case 11003: case 11004:
        case 11005:
            return true;
        default:
            return false;
    }
}

}  // namespace protocol
}  // namespace cm
