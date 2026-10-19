#include "logic/expiry_job.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "db/mysql_conn.h"
#include "db/mysql_pool.h"
#include "db/transfer_dao.h"

namespace cm {
namespace logic {
namespace {

const std::size_t kMaxTasksPerRound = 200;
const std::size_t kChunkDeleteBatch = 500;
const std::chrono::milliseconds kRoundBudget(2000);

bool isDots(const std::string& name) {
    return name == "." || name == "..";
}

bool dirIsEmpty(const std::string& path) {
    DIR* dir = ::opendir(path.c_str());
    if (dir == NULL) {
        return false;
    }
    bool empty = true;
    struct dirent* entry = NULL;
    while ((entry = ::readdir(dir)) != NULL) {
        if (!isDots(entry->d_name)) {
            empty = false;
            break;
        }
    }
    ::closedir(dir);
    return empty;
}

}  // namespace

ExpiryJob::ExpiryJob(cm::db::MysqlPool& mysql,
                     const std::string& dataDir,
                     DirDeleter deleter)
    : m_mysql(mysql), m_dataDir(dataDir), m_deleter(deleter) {
}

ErrorCode ExpiryJob::runOnce(std::size_t* expiredCount) {
    if (expiredCount != NULL) {
        *expiredCount = 0;
    }

    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        return (m_mysql.lastError() == ErrorCode::Ok) ? ErrorCode::DbPoolTimeout
                                                      : m_mysql.lastError();
    }

    cm::db::TransferDao dao(*conn);
    std::vector<cm::db::TransferTaskRow> tasks;
    ErrorCode rc = dao.findExpired(&tasks, kMaxTasksPerRound);
    if (rc != ErrorCode::Ok) {
        return rc;
    }

    const std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
    std::size_t processed = 0;

    for (std::size_t i = 0; i < tasks.size(); ++i) {
        if (processed > 0 &&
            std::chrono::steady_clock::now() - start >= kRoundBudget) {
            break;  // 超时留到下一轮
        }

        const cm::db::TransferTaskRow& task = tasks[i];

        // 条件 UPDATE：只对 0/1/2 生效，7 永远不在这里被置 6。
        std::vector<std::uint8_t> from;
        from.push_back(0);
        from.push_back(1);
        from.push_back(2);
        bool changed = false;
        rc = dao.markStatus(task.userId, task.taskId, from, 6, 0, &changed);
        if (rc != ErrorCode::Ok) {
            return rc;
        }
        if (!changed) {
            continue;  // 被并发请求改过状态（例如刚完成合并）
        }

        // 先置 6 再删行：此后的分片请求都会被 3007 拒绝。
        // 已置 6 的任务必须删完，否则下一轮不会再扫到它。
        for (;;) {
            std::size_t deleted = 0;
            rc = dao.deleteChunks(task.taskId, kChunkDeleteBatch, &deleted);
            if (rc != ErrorCode::Ok) {
                // TODO(log): 记 WARN，任务已置 6，残留分片行待人工/对账处理。
                break;
            }
            if (deleted < kChunkDeleteBatch) {
                break;
            }
        }

        if (m_deleter) {
            m_deleter(m_dataDir + "/tmp/" + task.taskId);
        }
        ++processed;
    }

    if (expiredCount != NULL) {
        *expiredCount = processed;
    }
    return ErrorCode::Ok;
}

ErrorCode ExpiryJob::recoverOnStartup(std::uint64_t* affected) {
    if (affected != NULL) {
        *affected = 0;
    }

    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        return (m_mysql.lastError() == ErrorCode::Ok) ? ErrorCode::DbPoolTimeout
                                                      : m_mysql.lastError();
    }
    cm::db::TransferDao dao(*conn);
    return dao.resetMergingOnStartup(affected);
}

ErrorCode ExpiryJob::cleanStaleTmp(std::uint64_t maxAgeSeconds,
                                   std::size_t* removedDirs) {
    if (removedDirs != NULL) {
        *removedDirs = 0;
    }

    const std::string tmpRoot = m_dataDir + "/tmp";
    DIR* root = ::opendir(tmpRoot.c_str());
    if (root == NULL) {
        return ErrorCode::Ok;  // 目录不存在：无可清理
    }

    const std::time_t now = std::time(NULL);

    struct dirent* entry = NULL;
    while ((entry = ::readdir(root)) != NULL) {
        if (isDots(entry->d_name)) {
            continue;
        }
        const std::string taskDir = tmpRoot + "/" + entry->d_name;
        struct stat st;
        if (::stat(taskDir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }

        // 1) 删除超过 1h 的 merged.bin*（先收集再删，避免边遍历边改目录）。
        std::vector<std::string> stale;
        DIR* sub = ::opendir(taskDir.c_str());
        if (sub != NULL) {
            struct dirent* file = NULL;
            while ((file = ::readdir(sub)) != NULL) {
                const std::string name = file->d_name;
                if (isDots(name) || name.compare(0, 10, "merged.bin") != 0) {
                    continue;
                }
                const std::string path = taskDir + "/" + name;
                struct stat fileStat;
                if (::stat(path.c_str(), &fileStat) == 0 &&
                    S_ISREG(fileStat.st_mode) &&
                    now - fileStat.st_mtime >
                        static_cast<std::time_t>(maxAgeSeconds)) {
                    stale.push_back(path);
                }
            }
            ::closedir(sub);
        }
        for (std::size_t i = 0; i < stale.size(); ++i) {
            ::unlink(stale[i].c_str());
        }

        // 2) 空任务目录直接删。
        if (dirIsEmpty(taskDir)) {
            if (::rmdir(taskDir.c_str()) == 0 && removedDirs != NULL) {
                ++(*removedDirs);
            }
        }
    }
    ::closedir(root);
    return ErrorCode::Ok;
}

}  // namespace logic
}  // namespace cm
