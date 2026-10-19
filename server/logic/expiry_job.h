#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "cm/base/error_code.h"

namespace cm {

namespace db {
class MysqlPool;
}  // namespace db

namespace logic {

// 上传任务过期清理 + 启动恢复。
// runOnce 只做 DB 与“删除目录”的投递，不做文件 IO；目录由调用方注入的
// DirDeleter 投到 IO 池执行。
class ExpiryJob {
public:
    using DirDeleter = std::function<void(const std::string&)>;

    ExpiryJob(cm::db::MysqlPool& mysql,
              const std::string& dataDir,
              DirDeleter deleter);

    ExpiryJob(const ExpiryJob&) = delete;
    ExpiryJob& operator=(const ExpiryJob&) = delete;

    // 单轮最多 200 任务、2s；超时留到下一轮。只扫 0/1/2，7 不参与过期清理。
    // expiredCount 返回本轮成功置 6 的任务数（可为 NULL）。
    cm::ErrorCode runOnce(std::size_t* expiredCount);

    // 启动恢复：UPDATE tr_status=2 WHERE tr_status=7。
    cm::ErrorCode recoverOnStartup(std::uint64_t* affected);

    // 清理 1h 前的 tmp/<task>/merged.bin* 与空任务目录（启动阶段调用）。
    cm::ErrorCode cleanStaleTmp(std::uint64_t maxAgeSeconds,
                                std::size_t* removedDirs);

private:
    cm::db::MysqlPool& m_mysql;
    std::string m_dataDir;
    DirDeleter m_deleter;
};

}  // namespace logic
}  // namespace cm
