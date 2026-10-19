#include "logic/rate_limiter.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace cm {
namespace logic {
namespace {

const std::uint64_t kMaxWaitMs = 2000;  // 等待超过 2s 直接回 3006

TimePoint steadyNow() {
    return std::chrono::steady_clock::now();
}

}  // namespace

TokenBucket::TokenBucket(std::uint64_t capacityBytes,
                         std::uint64_t rateBytesPerSec,
                         Clock clock)
    : m_capacity(capacityBytes),
      m_rate(rateBytesPerSec),
      m_tokens(static_cast<double>(capacityBytes)) {
    if (clock) {
        m_clock = std::move(clock);
    } else {
        m_clock = Clock(&steadyNow);
    }
    m_lastRefill = m_clock();
}

void TokenBucket::refill() {
    const TimePoint now = m_clock();
    if (now <= m_lastRefill) {
        return;  // 时钟未前进（含 fake clock 未推进）
    }
    const double elapsedSec =
        std::chrono::duration_cast<std::chrono::duration<double> >(now - m_lastRefill)
            .count();
    m_tokens = std::min(static_cast<double>(m_capacity),
                        m_tokens + elapsedSec * static_cast<double>(m_rate));
    m_lastRefill = now;
}

cm::ErrorCode TokenBucket::tryAcquire(std::uint64_t lenBytes,
                                      std::uint64_t* waitMs) {
    if (waitMs != NULL) {
        *waitMs = 0;
    }

    refill();
    if (m_tokens >= static_cast<double>(lenBytes)) {
        m_tokens -= static_cast<double>(lenBytes);
        return ErrorCode::Ok;
    }

    if (m_rate == 0) {
        return ErrorCode::RateLimited;
    }

    const double need = static_cast<double>(lenBytes) - m_tokens;
    const double waitMsD = need * 1000.0 / static_cast<double>(m_rate);
    const std::uint64_t waitMillis =
        static_cast<std::uint64_t>(std::ceil(waitMsD));
    if (waitMs != NULL) {
        *waitMs = waitMillis;
    }
    if (waitMillis > kMaxWaitMs) {
        return ErrorCode::RateLimited;
    }
    return ErrorCode::Timeout;  // 等待 waitMillis 后重试
}

}  // namespace logic
}  // namespace cm
