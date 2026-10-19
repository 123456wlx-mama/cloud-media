#pragma once

#include <cstddef>
#include <cstdint>

#include "cm/base/error_code.h"

namespace cm {
namespace db {
class MysqlPool;
}  // namespace db

namespace logic {

// 播放记录清理：p_file_id 在 t_file 中已不存在的孤儿行。
// 每月 1 日由 ServerApp 触发；单轮上限由构造参数决定（默认 500 行）。
class PlayCleanupJob {
public:
    explicit PlayCleanupJob(cm::db::MysqlPool& mysql,
                            std::size_t batch = 500);

    PlayCleanupJob(const PlayCleanupJob&) = delete;
    PlayCleanupJob& operator=(const PlayCleanupJob&) = delete;

    cm::ErrorCode runOnce(std::uint64_t* deleted);

private:
    cm::db::MysqlPool& m_mysql;
    std::size_t m_batch;
};

}  // namespace logic
}  // namespace cm
