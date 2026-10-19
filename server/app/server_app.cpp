#include "server_app.h"

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "cm/config/server_config.h"
#include "logic/expiry_job.h"
#include "logic/jobs/orphan_gc_job.h"
#include "logic/jobs/play_cleanup_job.h"
#include "logic/jobs/share_expiry_job.h"
#include "logic/upload_fs.h"

namespace cm {
namespace app {
namespace {

const std::uint64_t kStaleTmpAgeSeconds = 3600;
// 计划里的固定时刻：分享过期每日 03:10，GC 每日 04:00（分享之后），
// 播放记录清理每月 1 日 00:05。
const int kShareHour = 3;
const int kShareMinute = 10;
const int kGcHour = 4;
const int kGcMinute = 0;

std::int64_t msUntilDaily(int hour, int minute) {
    const std::time_t now = std::time(NULL);
    std::tm target;
    std::memset(&target, 0, sizeof(target));
    ::localtime_r(&now, &target);
    target.tm_hour = hour;
    target.tm_min = minute;
    target.tm_sec = 0;

    std::time_t next = std::mktime(&target);
    if (next <= now) {
        target.tm_mday += 1;
        next = std::mktime(&target);
    }
    return static_cast<std::int64_t>(next - now) * 1000;
}

std::int64_t msUntilMonthly() {
    const std::time_t now = std::time(NULL);
    std::tm target;
    std::memset(&target, 0, sizeof(target));
    ::localtime_r(&now, &target);
    target.tm_mday = 1;
    target.tm_hour = 0;
    target.tm_min = 5;
    target.tm_sec = 0;

    std::time_t next = std::mktime(&target);
    if (next <= now) {
        target.tm_mon += 1;
        next = std::mktime(&target);
    }
    return static_cast<std::int64_t>(next - now) * 1000;
}

// 业务池里执行；ENOENT 视为已删除，避免残留 .crc / 已缺失 blob 卡住行。
bool unlinkBlob(const std::string& path) {
    if (::unlink(path.c_str()) == 0) {
        return true;
    }
    return errno == ENOENT;
}

}  // namespace

const std::int64_t ServerApp::kDefaultExpiryIntervalMs;

ServerApp::ServerApp(const cm::ServerConfig& cfg,
                     cm::db::MysqlPool& mysql,
                     net::TimerQueue& timers,
                     cm::ThreadPool& workers,
                     std::int64_t expiryTickMs)
    : m_cfg(cfg),
      m_mysql(mysql),
      m_timers(timers),
      m_workers(workers),
      m_ioPool(2, "cm-io"),
      m_expiryTickMs(expiryTickMs),
      m_expiryRunning(false),
      m_gcRunning(false),
      m_shareRunning(false),
      m_playRunning(false),
      m_started(false) {
    m_expiry.reset(new cm::logic::ExpiryJob(
        m_mysql, m_cfg.dataDir, [this](const std::string& dir) {
            // tick 线程只投递；真正的 rm -r 由 IO 池执行。
            if (!m_ioPool.run([dir]() { cm::logic::removeTree(dir); })) {
                // IO 池已停止：目录留给下次启动清理。
            }
        }));
    m_gc.reset(new cm::logic::OrphanGcJob(m_mysql, m_cfg.dataDir, unlinkBlob));
    m_shareExpiry.reset(new cm::logic::ShareExpiryJob(m_mysql));
    m_playCleanup.reset(new cm::logic::PlayCleanupJob(m_mysql));
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

    if (m_expiryTickMs <= 0) {
        m_expiryTickMs = kDefaultExpiryIntervalMs;
    }
    m_tick = m_timers.runEvery(m_expiryTickMs, [this]() { onTick(); });
    scheduleDaily(&m_gcTimer, kGcHour, kGcMinute, [this]() { onGcTick(); });
    scheduleDaily(&m_shareTimer, kShareHour, kShareMinute,
                  [this]() { onShareExpiryTick(); });
    scheduleMonthly(&m_playTimer, [this]() { onPlayCleanupTick(); });
    m_started = true;
    return true;
}

void ServerApp::stop() {
    if (m_started) {
        m_timers.cancel(m_tick);
        m_timers.cancel(m_gcTimer);
        m_timers.cancel(m_shareTimer);
        m_timers.cancel(m_playTimer);
        m_started = false;
    }
    m_ioPool.stop();
}

void ServerApp::triggerExpiryTick() {
    onTick();
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

void ServerApp::onGcTick() {
    bool expected = false;
    if (!m_gcRunning.compare_exchange_strong(expected, true)) {
        return;
    }
    if (!m_workers.run([this]() {
            if (m_gc) {
                m_gc->runOnce(NULL, NULL);
            }
            m_gcRunning.store(false);
        })) {
        m_gcRunning.store(false);
    }
}

void ServerApp::onShareExpiryTick() {
    bool expected = false;
    if (!m_shareRunning.compare_exchange_strong(expected, true)) {
        return;
    }
    if (!m_workers.run([this]() {
            if (m_shareExpiry) {
                m_shareExpiry->runOnce(NULL);
            }
            m_shareRunning.store(false);
        })) {
        m_shareRunning.store(false);
    }
}

void ServerApp::onPlayCleanupTick() {
    bool expected = false;
    if (!m_playRunning.compare_exchange_strong(expected, true)) {
        return;
    }
    if (!m_workers.run([this]() {
            if (m_playCleanup) {
                m_playCleanup->runOnce(NULL);
            }
            m_playRunning.store(false);
        })) {
        m_playRunning.store(false);
    }
}

void ServerApp::scheduleDaily(net::TimerId* id, int hour, int minute,
                              std::function<void()> fn) {
    *id = m_timers.runAfter(msUntilDaily(hour, minute),
                            [this, id, hour, minute, fn]() {
                                fn();
                                scheduleDaily(id, hour, minute, fn);
                            });
}

void ServerApp::scheduleMonthly(net::TimerId* id, std::function<void()> fn) {
    *id = m_timers.runAfter(msUntilMonthly(), [this, id, fn]() {
        fn();
        scheduleMonthly(id, fn);
    });
}

}  // namespace app
}  // namespace cm
