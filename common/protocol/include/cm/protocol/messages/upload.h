#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cm {
namespace protocol {

// 10030/10031 的坏片下标上限（06 §4.5）：超出截断并置 code=1001（由服务层设置）。
constexpr std::size_t kMaxBadIndexes = 1024;
// 位图上限：total_chunks <= 524288 时 ceil(n/8) <= 64KB（06 §4.4）。
constexpr std::uint32_t kMaxChunkCount = 524288;

// 10004 上传初始化。
struct UploadInitRq {
    std::uint64_t parentId;
    std::string fileName;
    std::uint64_t totalSize;
    std::string fileMd5;
    std::uint32_t chunkSize;
    std::uint32_t totalChunks;
    std::uint8_t flags;
    std::uint32_t reqId;

    UploadInitRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10005。
struct UploadInitRs {
    std::uint32_t code;
    std::uint8_t result;  // 0=需要上传 1=秒传命中
    std::string taskId;
    std::uint64_t fileId;
    std::uint64_t ufId;
    std::string fileName;
    std::uint32_t chunkSize;
    std::uint32_t totalChunks;
    std::uint64_t expireAtMs;

    UploadInitRs();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10006：bulk 帧，payload = 字段 + 原始分片字节。
struct UploadChunkRq {
    std::string taskId;
    std::uint32_t chunkIndex;
    std::uint64_t offset;
    std::string chunkMd5;
    std::string data;

    UploadChunkRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10007。
struct UploadChunkRs {
    std::uint32_t code;
    std::uint32_t chunkIndex;
    std::uint8_t duplicate;
    std::uint32_t doneChunks;
    std::uint64_t doneBytes;
    std::uint32_t nextHint;  // 0xFFFFFFFF=已齐

    UploadChunkRs();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10017 秒传预检。
struct QuickUploadRq {
    std::string fileMd5;
    std::uint64_t totalSize;

    QuickUploadRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10024。
struct QuickUploadRs {
    std::uint32_t code;
    std::uint8_t hit;
    std::uint64_t fileId;
    std::uint64_t size;

    QuickUploadRs();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10028 续传查询。
struct ContinueUploadRq {
    std::string taskId;

    ContinueUploadRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10029：bitmap 为 ceil(total_chunks/8) 字节，LSB-first。
struct ContinueUploadRs {
    std::uint32_t code;
    std::string taskId;
    std::uint8_t status;
    std::uint32_t chunkSize;
    std::uint32_t totalChunks;
    std::uint32_t doneChunks;
    std::uint64_t doneBytes;
    std::string bitmap;
    std::uint64_t expireAtMs;

    ContinueUploadRs();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10030 触发合并。
struct UploadCompleteRq {
    std::string taskId;

    UploadCompleteRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10031：bad_count 由 badIndexes.size() 派生，不单列字段。
struct UploadCompleteRs {
    std::uint32_t code;
    std::uint64_t fileId;
    std::uint64_t ufId;
    std::string fileName;
    std::string fileMd5;
    std::uint64_t size;
    std::vector<std::uint32_t> badIndexes;

    UploadCompleteRs();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

}  // namespace protocol
}  // namespace cm
