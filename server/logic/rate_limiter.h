#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

#include "cm/base/error_code.h"

namespace cm {
namespace logic {

// 默认限速参数（D-5）：连接级与用户级各一个桶；D-6 据此实例化。
constexpr std::uint64_t kConnTokenCapacity = 4 * 1024 * 1024;     // 4MB
constexpr std::uint64_t kConnTokenRatePerSec = 2 * 1024 * 1024;   // 2MB/s
constexpr std::uint64_t kUserTokenCapacity = 16 * 1024 * 1024;    // 16MB
constexpr std::uint64_t kUserTokenRatePerSec = 8 * 1024 * 1024;   // 8MB/s

using TimePoint = std::chrono::steady_clock::time_point;

// 时钟抽象：默认 steady_clock::now()；单测注入可控时钟，不必真实等待。
using Clock = std::function<TimePoint()>;

// 令牌桶：用 steady_clock 补充令牌，不依赖系统时钟跳变。
// 容量/速率单位都是字节（与 4MB 块对齐）。
class TokenBucket {
public:
    TokenBucket(std::uint64_t capacityBytes,
                std::uint64_t rateBytesPerSec,
                Clock clock = Clock());

    TokenBucket(const TokenBucket&) = delete;
    TokenBucket& operator=(const TokenBucket&) = delete;

    // 尝试消费 lenBytes 字节。
    //   Ok          —— 令牌充足，已扣减，*waitMs=0。
    //   Timeout     —— 令牌不足但等待 ≤ 2s，*waitMs 为需等待毫秒数（调用方 runAfter 重试，不 sleep）。
    //   RateLimited —— 令牌不足且等待 > 2s（或速率为 0），应回 3006。
    cm::ErrorCode tryAcquire(std::uint64_t lenBytes, std::uint64_t* waitMs);

    std::uint64_t capacityBytes() const { return m_capacity; }
    std::uint64_t rateBytesPerSec() const { return m_rate; }

private:
    void refill();

    std::uint64_t m_capacity;
    std::uint64_t m_rate;  // bytes/s
    double m_tokens;
    TimePoint m_lastRefill;
    Clock m_clock;
};

}  // namespace logic
}  // namespace cm
