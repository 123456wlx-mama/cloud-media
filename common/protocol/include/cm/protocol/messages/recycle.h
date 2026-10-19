#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cm {
namespace protocol {

// 10025 软删除。
struct DeleteFileRq {
    std::uint64_t ufId;

    DeleteFileRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10026。
struct DeleteFileRs {
    std::uint32_t code;
    std::uint8_t deleted;
    std::uint8_t already;

    DeleteFileRs();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10034 回收站列表。
struct RecycleListRq {
    std::uint32_t limit;
    std::uint64_t cursorDeletedMs;
    std::uint64_t cursorId;

    RecycleListRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

struct RecycleItem {
    std::uint64_t ufId;
    std::string name;
    std::uint8_t isDir;
    std::uint64_t size;
    std::uint64_t deletedAtMs;
    std::uint64_t parentId;

    RecycleItem();
};

// 10035：count 由 items.size() 派生。
struct RecycleListRs {
    std::uint32_t code;
    std::vector<RecycleItem> items;
    std::uint8_t hasMore;
    std::uint64_t nextDeletedMs;
    std::uint64_t nextId;

    RecycleListRs();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10036 恢复。
struct RestoreRq {
    std::uint64_t ufId;
    std::string newName;
    std::uint8_t autoRename;

    RestoreRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10037。
struct RestoreRs {
    std::uint32_t code;
    std::uint64_t ufId;
    std::uint64_t parentId;
    std::string name;
    std::uint8_t renamed;

    RestoreRs();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10038 彻底删除。
struct PurgeRq {
    std::uint64_t ufId;
    std::uint8_t confirm;  // 必须=1

    PurgeRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10039。
struct PurgeRs {
    std::uint32_t code;
    std::uint32_t filesDeleted;
    std::uint32_t dirsDeleted;
    std::uint32_t blobsOrphaned;

    PurgeRs();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

}  // namespace protocol
}  // namespace cm
