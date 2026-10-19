#pragma once

#include <atomic>
#include <cstdint>
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
}  // namespace logic

namespace app {

// 启动顺序的唯一入口：恢复 7→2 → 清理过期 tmp → 注册过期扫描 tick。
// 监听端口仍由 main 在 start() 之后执行（失败则整个进程退出）。
class ServerApp {
public:
    ServerApp(const cm::ServerConfig& cfg,
              cm::db::MysqlPool& mysql,
              net::TimerQueue& timers,
              cm::ThreadPool& workers);

    ~ServerApp();

    ServerApp(const ServerApp&) = delete;
    ServerApp& operator=(const ServerApp&) = delete;

    // 失败只可能是 DB 恢复失败；tmp 清理失败不阻断启动。
    bool start(std::string* error);

    // 取消 tick、停止 IO 池（会 drain 已投递的目录删除）。
    void stop();

private:
    void onTick();

    const cm::ServerConfig& m_cfg;
    cm::db::MysqlPool& m_mysql;
    net::TimerQueue& m_timers;
    cm::ThreadPool& m_workers;
    cm::ThreadPool m_ioPool;
    std::unique_ptr<cm::logic::ExpiryJob> m_expiry;
    net::TimerId m_tick;
    std::atomic<bool> m_expiryRunning;
    bool m_started;
};

}  // namespace app
}  // namespace cm
