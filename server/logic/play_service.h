#pragma once

#include <cstdint>

#include "cm/base/error_code.h"
#include "cm/protocol/messages/play.h"

namespace cm {
namespace db {
class MysqlPool;
}  // namespace db

namespace logic {

// 播放进度上报（10032/10033）。
// 只做可见性校验与 LWW 落库；不做 Redis 缓存、不做服务端节流（06 §4.19：
// 客户端 10s 节流一次）。
class PlayService {
public:
    explicit PlayService(cm::db::MysqlPool& mysql);

    PlayService(const PlayService&) = delete;
    PlayService& operator=(const PlayService&) = delete;

    // 校验 f_id 是当前用户任意目录下的活跃文件节点 → 落 LWW。
    // 不可见/不存在回 1002；position>duration（duration>0）回 1001；
    // 成功 rs.saved=1。
    cm::ErrorCode report(std::uint64_t uid,
                         const cm::protocol::PlayReportRq& rq,
                         cm::protocol::PlayReportRs* out);

private:
    cm::db::MysqlPool& m_mysql;
};

}  // namespace logic
}  // namespace cm
