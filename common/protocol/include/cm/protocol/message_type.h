#pragma once

#include <cstdint>

namespace cm {
namespace protocol {

// 类型号与 docs/specs/06-业务设计.md 第 3 节逐项一致。
// 10011 / 10024 是 02 里废弃的槽位，Phase 5 复用为 DownloadDataRs / QuickUploadRs；
// 9007（ERR_DEPRECATED）保留给未来真正退役的类型。
enum class MessageType : std::uint16_t {
    RegisterRq = 10000,
    RegisterRs = 10001,
    LoginRq = 10002,
    LoginRs = 10003,
    UploadInitRq = 10004,
    UploadInitRs = 10005,
    UploadChunkRq = 10006,   // bulk 帧：4MB 分片数据，CRC 由业务线程校验
    UploadChunkRs = 10007,
    ListFileRq = 10008,
    ListFileRs = 10009,
    DownloadOpenRq = 10010,
    DownloadDataRs = 10011,  // 原 DownloadFolderRq（废弃槽位复用）：纯 raw 块
    DownloadOpenRs = 10012,
    DownloadRangeRq = 10013,
    DownloadStatRs = 10014,
    AddFolderRq = 10015,
    AddFolderRs = 10016,
    QuickUploadRq = 10017,
    ShareCreateRq = 10018,
    ShareCreateRs = 10019,
    ShareListRq = 10020,
    ShareListRs = 10021,
    ShareOpenRq = 10022,
    ShareOpenRs = 10023,
    QuickUploadRs = 10024,   // 原 FolderHeaderRq（废弃槽位复用）
    DeleteFileRq = 10025,
    DeleteFileRs = 10026,
    DownloadStatRq = 10027,
    ContinueUploadRq = 10028,
    ContinueUploadRs = 10029,
    UploadCompleteRq = 10030,
    UploadCompleteRs = 10031,
    PlayReportRq = 10032,
    PlayReportRs = 10033,
    RecycleListRq = 10034,
    RecycleListRs = 10035,
    RestoreRq = 10036,
    RestoreRs = 10037,
    PurgeRq = 10038,
    PurgeRs = 10039,
    DownloadDoneRq = 10040,
    DownloadDoneRs = 10041,
    ShareCancelRq = 10042,
    ShareCancelRs = 10043,

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
        case 10030: case 10031: case 10032: case 10033: case 10034:
        case 10035: case 10036: case 10037: case 10038: case 10039:
        case 10040: case 10041: case 10042: case 10043:
        case 11000: case 11001: case 11002: case 11003: case 11004:
        case 11005:
            return true;
        default:
            return false;
    }
}

}  // namespace protocol
}  // namespace cm
