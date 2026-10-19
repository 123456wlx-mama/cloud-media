#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cm {
namespace protocol {

struct FileItem {
    std::uint64_t ufId;
    std::uint64_t fId;  // 目录为 0
    std::string name;
    std::uint8_t isDir;
    std::uint64_t size;
    std::string md5;  // 目录为空
    std::uint64_t createdAtMs;
    std::uint64_t updatedAtMs;

    FileItem();
};

// 10008 目录分页。
struct ListFileRq {
    std::uint64_t parentId;
    std::uint8_t sort;  // 0=名称升序 1=创建时间降序 2=更新时间降序
    std::uint32_t limit;
    std::string cursorKey;
    std::uint64_t cursorId;

    ListFileRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10009：count 由 items.size() 派生。
struct ListFileRs {
    std::uint32_t code;
    std::vector<FileItem> items;
    std::uint8_t hasMore;
    std::string nextKey;
    std::uint64_t nextId;
    std::uint64_t totalCount;  // 仅首页；未知为 0xFFFFFFFFFFFFFFFF

    ListFileRs();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

}  // namespace protocol
}  // namespace cm
