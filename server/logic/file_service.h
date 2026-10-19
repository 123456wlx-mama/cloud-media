#pragma once

#include <cstddef>
#include <cstdint>

#include "cm/base/error_code.h"
#include "cm/protocol/messages/list.h"

namespace cm {
namespace db {
class MysqlPool;
}  // namespace db

namespace logic {

// 文件列表（10008/10009）服务层：只做参数校验、游标映射与 wire 组装。
// 不碰 Redis/缓存；存储读写经 FileDao 完成。
class FileService {
public:
    static const std::uint64_t kUnknownTotalCount = 0xFFFFFFFFFFFFFFFFULL;
    static const std::size_t kDefaultLimit = 50;
    static const std::size_t kMaxLimit = 200;
    static const std::uint64_t kDefaultMaxTotalCount = 10000;

    // maxTotalCount 可注入（测试用较小值覆盖“超过上限即截断”）。
    explicit FileService(cm::db::MysqlPool& mysql,
                         std::uint64_t maxTotalCount = kDefaultMaxTotalCount);

    FileService(const FileService&) = delete;
    FileService& operator=(const FileService&) = delete;

    // 10008：sort 0/1/2 → NameAsc/CreatedDesc/UpdatedDesc；
    // limit 0 视为 50，>200 回 1001；非首页游标按排序类型校验，非法回 1001。
    // hasMore/nextKey/nextId 透传 DAO；totalCount 仅首页精确统计（上限截断），
    // 非首页或 COUNT 超时/失败为 kUnknownTotalCount。
    cm::ErrorCode list(std::uint64_t uid,
                       const cm::protocol::ListFileRq& rq,
                       cm::protocol::ListFileRs* out);

private:
    cm::db::MysqlPool& m_mysql;
    std::uint64_t m_maxTotalCount;
};

}  // namespace logic
}  // namespace cm
