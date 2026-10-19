#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "cm/base/error_code.h"
#include "cm/protocol/messages/share.h"

namespace cm {

namespace db {
class MysqlPool;
}  // namespace db

namespace cache {
class CacheStore;
class RedisPool;
}  // namespace cache

namespace logic {

// 分享码生成器（可注入，供碰撞重试用例固定序列）。
using ShareCodeGenerator = std::function<std::string()>;

// 分享服务：10018 创建、10022 访问、10042 取消。
// 只经 ShareDao/FileDao 与 CacheStore/RedisPool 访问存储，不出现 MySQL/Redis C API。
// 缓存：share/{code} 5min±10%（fail-open 回源）；miss 60s；
//       取消/改名/删除后失效；提取码失败计数 fail-closed。
class ShareService {
public:
    // 31 字符表（去掉 0/O/1/I/L）+ 拒绝采样，6 位，统一大写。
    static std::string generateCode();

    ShareService(cm::db::MysqlPool& mysql,
                 cm::cache::CacheStore& cache,
                 cm::cache::RedisPool& redis,
                 ShareCodeGenerator codeGen = ShareCodeGenerator());

    ShareService(const ShareService&) = delete;
    ShareService& operator=(const ShareService&) = delete;

    // 10018：校验本人节点 → req 幂等 → 生成码（1062 重试 5 次）→ 落库 → 缓存。
    // 成功返回 Ok 且 out->code==0；业务失败时 out->code 为对应数值（1001/1002/1003/4001~4008）。
    cm::ErrorCode create(std::uint64_t uid,
                         const cm::protocol::ShareCreateRq& rq,
                         cm::protocol::ShareCreateRs* out);

    // 10022：归一化码 → 缓存/回源 → 校验状态与过期 → 提取码（fail-closed）→ 元信息 + share_ticket。
    // clientKey 是提取码失败计数的作用域（通常为连接 IP），Redis 挂时带提取码分享回 4008。
    cm::ErrorCode open(const cm::protocol::ShareOpenRq& rq,
                       const std::string& clientKey,
                       cm::protocol::ShareOpenRs* out);

    // 10020：我的分享列表（keyset 游标 (created_at, s_id)）。
    cm::ErrorCode list(std::uint64_t uid,
                       const cm::protocol::ShareListRq& rq,
                       cm::protocol::ShareListRs* out);

    // 10042：取消分享（幂等），成功后失效 share/{code} 缓存。
    cm::ErrorCode cancel(std::uint64_t uid,
                         const cm::protocol::ShareCancelRq& rq,
                         cm::protocol::ShareCancelRs* out);

private:
    std::string nextCode() const;

    cm::db::MysqlPool& m_mysql;
    cm::cache::CacheStore& m_cache;
    cm::cache::RedisPool& m_redis;
    ShareCodeGenerator m_codeGen;
};

}  // namespace logic
}  // namespace cm
