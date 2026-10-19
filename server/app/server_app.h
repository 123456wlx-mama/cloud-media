#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "cm/base/thread_pool.h"
#include "net/TimerQueue.h"

namespace cm {

struct ServerConfig;

namespace db {
class MysqlPool;
}  // namespace db

namespace logic {
class ExpiryJob;
class OrphanGcJob;
class PlayCleanupJob;
class ShareExpiryJob;
}  // namespace logic

namespace app {

// 启动顺序的唯一入口：恢复 7→2 → 清理过期 tmp → 注册过期扫描 tick。
// 监听端口仍由 main 在 start() 之后执行（失败则整个进程退出）。
class ServerApp {
public:
    static const std::int64_t kDefaultExpiryIntervalMs = 5 * 60 * 1000;

    // expiryTickMs 可注入：生产用 5min；计时用例用 1ms/1s。
    ServerApp(const cm::ServerConfig& cfg,
              cm::db::MysqlPool& mysql,
              net::TimerQueue& timers,
              cm::ThreadPool& workers,
              std::int64_t expiryTickMs = kDefaultExpiryIntervalMs);

    ~ServerApp();

    ServerApp(const ServerApp&) = delete;
    ServerApp& operator=(const ServerApp&) = delete;

    // 失败只可能是 DB 恢复失败；tmp 清理失败不阻断启动。
    bool start(std::string* error);

    // 取消 tick、停止 IO 池（会 drain 已投递的目录删除）。
    void stop();

    // 下载回退读块/合并等阻塞磁盘 IO 共用的 IO 池。
    cm::ThreadPool& ioPool() { return m_ioPool; }

    // 立即投递一轮过期扫描（只投业务池，不阻塞调用线程）。
    // 供运维手动触发与「tick 不阻塞 loop」计时用例使用。
    void triggerExpiryTick();

private:
    void onTick();
    void onGcTick();
    void onShareExpiryTick();
    void onPlayCleanupTick();
    // 每天 hour:minute 触发一次，执行后重新排下次。
    void scheduleDaily(net::TimerId* id, int hour, int minute,
                       std::function<void()> fn);
    // 每月 1 日 00:05 触发一次。
    void scheduleMonthly(net::TimerId* id, std::function<void()> fn);

    const cm::ServerConfig& m_cfg;
    cm::db::MysqlPool& m_mysql;
    net::TimerQueue& m_timers;
    cm::ThreadPool& m_workers;
    cm::ThreadPool m_ioPool;
    std::unique_ptr<cm::logic::ExpiryJob> m_expiry;
    std::unique_ptr<cm::logic::OrphanGcJob> m_gc;
    std::unique_ptr<cm::logic::ShareExpiryJob> m_shareExpiry;
    std::unique_ptr<cm::logic::PlayCleanupJob> m_playCleanup;
    net::TimerId m_tick;
    net::TimerId m_gcTimer;
    net::TimerId m_shareTimer;
    net::TimerId m_playTimer;
    std::int64_t m_expiryTickMs;
    std::atomic<bool> m_expiryRunning;
    std::atomic<bool> m_gcRunning;
    std::atomic<bool> m_shareRunning;
    std::atomic<bool> m_playRunning;
    bool m_started;
};

}  // namespace app
}  // namespace cm
