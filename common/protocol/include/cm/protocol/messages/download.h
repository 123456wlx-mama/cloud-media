#pragma once

#include <cstdint>
#include <string>

namespace cm {
namespace protocol {

// 10011 DownloadDataRs 是纯 raw payload（一个 4MB 块），没有字段结构，
// 因此这里不为它定义结构体；SEQ 与请求 10013 关联。

// 10010 打开下载。
struct DownloadOpenRq {
    std::uint64_t ufId;
    std::string shareTicket;
    std::uint32_t startBlock;

    DownloadOpenRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10012。
struct DownloadOpenRs {
    std::uint32_t code;
    std::uint64_t fId;
    std::uint64_t size;
    std::string fileMd5;
    std::uint32_t blockSize;
    std::uint32_t blockCount;
    std::string ticket;
    std::uint64_t ticketExpireMs;
    std::uint32_t startBlock;

    DownloadOpenRs();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10027 续传前校验远端元信息。
struct DownloadStatRq {
    std::uint64_t ufId;

    DownloadStatRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10014。
struct DownloadStatRs {
    std::uint32_t code;
    std::uint64_t fId;
    std::uint64_t size;
    std::string fileMd5;
    std::uint32_t blockSize;
    std::uint32_t blockCount;
    std::uint8_t state;
    std::uint64_t updatedAtMs;

    DownloadStatRs();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10013 取一个 4MB 块。
struct DownloadRangeRq {
    std::string ticket;
    std::uint64_t fId;
    std::uint32_t blockIndex;

    DownloadRangeRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10040 完成上报。
struct DownloadDoneRq {
    std::string ticket;
    std::uint64_t fId;
    std::uint64_t size;
    std::string fileMd5;
    std::uint8_t verifyOk;

    DownloadDoneRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10041。
struct DownloadDoneRs {
    std::uint32_t code;
    std::uint8_t accepted;

    DownloadDoneRs();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

}  // namespace protocol
}  // namespace cm
