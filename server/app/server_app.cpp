#include "server_app.h"

#include <cstdio>

#include "cm/config/server_config.h"
#include "logic/expiry_job.h"
#include "logic/upload_fs.h"

namespace cm {
namespace app {
namespace {

const std::int64_t kExpiryIntervalMs = 5 * 60 * 1000;
const std::uint64_t kStaleTmpAgeSeconds = 3600;

}  // namespace

ServerApp::ServerApp(const cm::ServerConfig& cfg,
                     cm::db::MysqlPool& mysql,
                     net::TimerQueue& timers,
                     cm::ThreadPool& workers)
    : m_cfg(cfg),
      m_mysql(mysql),
      m_timers(timers),
      m_workers(workers),
      m_ioPool(2, "cm-io"),
      m_expiryRunning(false),
      m_started(false) {
    m_expiry.reset(new cm::logic::ExpiryJob(
        m_mysql, m_cfg.dataDir, [this](const std::string& dir) {
            // tick 线程只投递；真正的 rm -r 由 IO 池执行。
            if (!m_ioPool.run([dir]() { cm::logic::removeTree(dir); })) {
                // IO 池已停止：目录留给下次启动清理。
            }
        }));
}

ServerApp::~ServerApp() {
    stop();
}

bool ServerApp::start(std::string* error) {
    if (m_started) {
        return true;
    }

    std::uint64_t recovered = 0;
    const cm::ErrorCode rc = m_expiry->recoverOnStartup(&recovered);
    if (rc != cm::ErrorCode::Ok) {
        if (error != NULL) {
            *error = "reset tr_status=7 failed";
        }
        return false;
    }

    std::size_t removed = 0;
    // 清理失败不阻断启动：只是多留一点临时文件。
    if (m_expiry->cleanStaleTmp(kStaleTmpAgeSeconds, &removed) !=
        cm::ErrorCode::Ok) {
        // TODO(log): 记 WARN（清理过期 tmp 失败）。
    }

    m_tick = m_timers.runEvery(kExpiryIntervalMs, [this]() { onTick(); });
    m_started = true;
    return true;
}

void ServerApp::stop() {
    if (m_started) {
        m_timers.cancel(m_tick);
        m_started = false;
    }
    m_ioPool.stop();
}

void ServerApp::onTick() {
    bool expected = false;
    if (!m_expiryRunning.compare_exchange_strong(expected, true)) {
        return;  // 上一轮未结束：跳过本次 tick
    }

    const bool submitted = m_workers.run([this]() {
        if (m_expiry) {
            m_expiry->runOnce(NULL);
        }
        m_expiryRunning.store(false);
    });
    if (!submitted) {
        m_expiryRunning.store(false);
    }
}

}  // namespace app
}  // namespace cm
