#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cm/base/error_code.h"

namespace cm {
namespace db {

class MysqlConn;

struct PlayRow {
    std::uint64_t playId;
    std::uint64_t userId;
    std::uint64_t fileId;
    std::uint64_t positionMs;
    std::uint64_t durationMs;
    bool finished;
    std::string updatedAt;

    PlayRow();
};

// t_play_record 的 DAO。唯一键 (p_user_id, p_file_id) 保证一行。
class PlayDao {
public:
    explicit PlayDao(MysqlConn& conn);

    PlayDao(const PlayDao&) = delete;
    PlayDao& operator=(const PlayDao&) = delete;

    // LWW：直接覆盖旧值，允许回退进度（06 §4.19），不用 MAX/IF 保留旧值。
    ErrorCode upsertLww(std::uint64_t userId,
                        std::uint64_t fileId,
                        std::uint64_t positionMs,
                        std::uint64_t durationMs,
                        bool finished);

    ErrorCode findByUserFile(std::uint64_t userId, std::uint64_t fileId, PlayRow* out);

    ErrorCode listRecent(std::uint64_t userId,
                         std::size_t limit,
                         std::vector<PlayRow>* out);

    // 清理孤儿播放记录：p_file_id 在 t_file 中已不存在；单轮最多 limit 行。
    ErrorCode deleteOrphanRecords(std::size_t limit, std::uint64_t* deleted);

private:
    MysqlConn& m_conn;
};

}  // namespace db
}  // namespace cm
