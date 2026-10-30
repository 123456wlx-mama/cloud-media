#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cm {
namespace protocol {

// 10018 创建分享。
struct ShareCreateRq {
    std::uint64_t ufId;
    std::uint8_t expirePolicy;  // 0=永久 1=1天 2=7天
    std::uint8_t accessMode;    // 0=无提取码 1=生成 4 位
    std::uint32_t reqId;

    ShareCreateRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10019。
struct ShareCreateRs {
    std::uint32_t code;
    std::string shareCode;
    std::string accessCode;
    std::uint8_t accessCodeRequired;
    std::uint64_t expireAtMs;  // 0=永久

    ShareCreateRs();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10020 我的分享。
struct ShareListRq {
    std::uint64_t cursorCreatedMs;
    std::uint64_t cursorId;
    std::uint32_t limit;

    ShareListRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

struct ShareListItem {
    std::string shareCode;
    std::string fileName;
    std::uint64_t size;
    std::uint8_t accessCodeRequired;
    std::uint8_t status;
    std::uint64_t expireAtMs;
    std::uint64_t createdAtMs;

    ShareListItem();
};

// 10021：count 由 items.size() 派生。
struct ShareListRs {
    std::uint32_t code;
    std::vector<ShareListItem> items;
    std::uint8_t hasMore;
    std::uint64_t nextCreatedMs;
    std::uint64_t nextId;

    ShareListRs();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10022 访问分享。
struct ShareOpenRq {
    std::string shareCode;
    std::string accessCode;

    ShareOpenRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10023。
struct ShareOpenRs {
    std::uint32_t code;
    std::uint64_t fId;
    std::string fileName;
    std::uint64_t size;
    std::string fileMd5;
    std::uint8_t isDir;
    std::uint64_t expireAtMs;
    std::string shareTicket;
    std::uint8_t accessCodeRequired;

    ShareOpenRs();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10044 转存：把分享的文件保存到自己网盘（新增用户节点 + blob 引用计数 +1，
// 不产生任何字节传输；秒传同款落库逻辑）。
struct ShareSaveRq {
    std::string shareCode;
    std::string accessCode;  // 分享带提取码时必须带；没有则空串
    std::uint64_t parentId;  // 保存到哪个目录（0 = 根）
    std::uint32_t reqId;     // 幂等键：重复提交只建一个节点

    ShareSaveRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10045。
struct ShareSaveRs {
    std::uint32_t code;
    std::uint64_t ufId;      // 新节点 ID
    std::string fileName;    // 最终落库名（同名自动加 "(1)"）
    std::uint64_t size;
    std::uint8_t reused;     // 1 = 同目录已有同名同内容节点，直接复用

    ShareSaveRs();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10042 取消分享。
struct ShareCancelRq {
    std::string shareCode;

    ShareCancelRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10043。
struct ShareCancelRs {
    std::uint32_t code;
    std::uint8_t cancelled;
    std::uint8_t already;

    ShareCancelRs();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

}  // namespace protocol
}  // namespace cm
