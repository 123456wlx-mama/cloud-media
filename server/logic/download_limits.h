#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "logic/rate_limiter.h"

namespace cm {
namespace logic {

// 下载流上限：全局 512、每用户 8。超限回 3006。
// 连接关闭/块完成必须归还槽位（release）。
class StreamLimiter {
public:
    StreamLimiter(std::size_t globalLimit = 512, std::size_t perUserLimit = 8)
        : m_globalLimit(globalLimit),
          m_perUserLimit(perUserLimit),
          m_global(0) {
    }

    // 尝试占一个活跃流槽位。成功 true；超限（全局或该用户）false，应回 3006。
    bool tryAcquire(std::uint64_t uid) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_global >= m_globalLimit) {
            return false;
        }
        std::unordered_map<std::uint64_t, std::size_t>::iterator it =
            m_perUser.find(uid);
        if (it != m_perUser.end() && it->second >= m_perUserLimit) {
            return false;
        }
        ++m_global;
        ++m_perUser[uid];
        return true;
    }

    // 归还槽位。连接关闭/块完成时调用。
    void release(std::uint64_t uid) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_global > 0) {
            --m_global;
        }
        std::unordered_map<std::uint64_t, std::size_t>::iterator it =
            m_perUser.find(uid);
        if (it == m_perUser.end()) {
            return;
        }
        if (it->second > 0) {
            --it->second;
        }
        if (it->second == 0) {
            m_perUser.erase(it);
        }
    }

    std::size_t activeStreams() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_global;
    }

    std::size_t activeStreamsFor(std::uint64_t uid) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::unordered_map<std::uint64_t, std::size_t>::const_iterator it =
            m_perUser.find(uid);
        return it == m_perUser.end() ? 0 : it->second;
    }

private:
    mutable std::mutex m_mutex;
    std::size_t m_globalLimit;
    std::size_t m_perUserLimit;
    std::size_t m_global;
    std::unordered_map<std::uint64_t, std::size_t> m_perUser;
};

// 慢客户端防护：sendfile 在途块 30s 零字节进展 → 判定应关闭。
// 只在「有在途块」时生效，避免误杀正常空闲连接。
class SlowClientGuard {
public:
    explicit SlowClientGuard(Clock clock = Clock())
        : m_clock(std::move(clock)), m_inFlight(false) {
        m_lastProgress = now();
    }

    // 块开始发送（每连接同一时刻最多 1 个在途块）。
    void beginBlock() {
        m_inFlight = true;
        m_lastProgress = now();
    }

    // 记录一次字节进展（sendfile 每轮写出的字节）。
    void progress() {
        m_lastProgress = now();
    }

    // 块发送完成/取消，回到空闲。
    void endBlock() {
        m_inFlight = false;
    }

    bool inFlight() const { return m_inFlight; }

    // 当前是否应因无进展被关闭（有在途块 && 距上次进展 ≥ 30s）。
    bool shouldClose() const {
        if (!m_inFlight) {
            return false;
        }
        return now() - m_lastProgress >= std::chrono::seconds(30);
    }

private:
    TimePoint now() const {
        return m_clock ? m_clock() : std::chrono::steady_clock::now();
    }

    Clock m_clock;
    bool m_inFlight;
    TimePoint m_lastProgress;
};

}  // namespace logic
}  // namespace cm
