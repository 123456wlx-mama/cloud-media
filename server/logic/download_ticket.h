#pragma once

#include <cstdint>
#include <string>

#include "cm/base/error_code.h"

namespace cm {

namespace cache {
class RedisPool;
class RedisConn;
}  // namespace cache

namespace logic {

struct DownloadTicketInfo {
    std::uint64_t uid;
    std::uint64_t fid;
    std::uint64_t hardExpMs;  // 硬上限（毫秒时间戳）
    std::uint32_t flags;      // 预留位图，当前 0

    DownloadTicketInfo();
};

// 下载票据：Redis value = uid|fid|hard_exp|flags。
// 首次签发 5min，取块时滑动续期 5min，但永不超过 hard_exp（签发 + 2h）。
// 校验失败一律 PermissionDenied(1004)，不泄露具体原因；Redis 错误返回
// RedisUnavailable(4005) / AuthUnavailable(4007) 且拒绝，不 fail-open。
class DownloadTicket {
public:
    explicit DownloadTicket(cm::cache::RedisPool& pool);

    DownloadTicket(const DownloadTicket&) = delete;
    DownloadTicket& operator=(const DownloadTicket&) = delete;

    // 生成 32B → 64 hex；expireAtMs 返回初始 5min 的绝对时间。
    cm::ErrorCode issue(std::uint64_t uid,
                        std::uint64_t fid,
                        std::string* token,
                        std::uint64_t* expireAtMs);

    // 校验成功后续期到 min(now+5min, hard_exp)。
    cm::ErrorCode touch(const std::string& token,
                        std::uint64_t uid,
                        std::uint64_t fid,
                        DownloadTicketInfo* out);

    // 只校验，不续期。
    cm::ErrorCode verify(const std::string& token,
                         std::uint64_t uid,
                         std::uint64_t fid,
                         DownloadTicketInfo* out);

private:
    cm::ErrorCode loadWith(cm::cache::RedisConn& conn,
                           const std::string& token,
                           std::uint64_t uid,
                           std::uint64_t fid,
                           DownloadTicketInfo* out,
                           std::string* rawValue = nullptr) const;

    cm::cache::RedisPool& m_pool;
};

}  // namespace logic
}  // namespace cm
