#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

#include "logic/download_limits.h"
#include "logic/rate_limiter.h"

namespace {

const std::uint64_t kChunkSize = 4194304;             // 4MB
const std::uint64_t kRate2MBps = 2 * 1024 * 1024;     // 2097152 B/s

// 可控 fake clock：测试手动推进，绝不真实等待。
cm::logic::Clock makeClock(cm::logic::TimePoint* now) {
    return [now]() { return *now; };
}

TEST(RateLimiterTest, TokenBucketRefill) {
    cm::logic::TimePoint fakeNow(cm::logic::TimePoint(std::chrono::seconds(0)));
    cm::logic::TokenBucket bucket(kChunkSize, kRate2MBps, makeClock(&fakeNow));

    std::uint64_t waitMs = 0;
    // 初始满桶：一次取 4MB 成功，令牌归零。
    EXPECT_EQ(cm::ErrorCode::Ok, bucket.tryAcquire(kChunkSize, &waitMs));
    EXPECT_EQ(0u, waitMs);

    // 桶空：取 2MB 需要等 1s。
    EXPECT_EQ(cm::ErrorCode::Timeout,
              bucket.tryAcquire(2 * 1024 * 1024, &waitMs));
    EXPECT_EQ(1000u, waitMs);

    // 推进 1s：补充 2MB，再取 2MB 成功。
    fakeNow += std::chrono::seconds(1);
    EXPECT_EQ(cm::ErrorCode::Ok,
              bucket.tryAcquire(2 * 1024 * 1024, &waitMs));
    EXPECT_EQ(0u, waitMs);
}

TEST(RateLimiterTest, TokenBucketWaitCalculated) {
    cm::logic::TimePoint fakeNow(cm::logic::TimePoint(std::chrono::seconds(0)));
    cm::logic::TokenBucket bucket(kChunkSize, kRate2MBps, makeClock(&fakeNow));

    std::uint64_t waitMs = 0;
    EXPECT_EQ(cm::ErrorCode::Ok, bucket.tryAcquire(kChunkSize, &waitMs));

    // 取 1MB → 需等 0.5s = 500ms。
    EXPECT_EQ(cm::ErrorCode::Timeout,
              bucket.tryAcquire(1 * 1024 * 1024, &waitMs));
    EXPECT_EQ(500u, waitMs);

    // 取 3MB → 需等 1.5s = 1500ms。
    EXPECT_EQ(cm::ErrorCode::Timeout,
              bucket.tryAcquire(3 * 1024 * 1024, &waitMs));
    EXPECT_EQ(1500u, waitMs);

    // 取 8MB（> 容量）→ 需等 4s > 2s → 3006。
    EXPECT_EQ(cm::ErrorCode::RateLimited,
              bucket.tryAcquire(8 * 1024 * 1024, &waitMs));
}

TEST(RateLimiterTest, TokenBucketRateZeroReturns3006) {
    cm::logic::TimePoint fakeNow(cm::logic::TimePoint(std::chrono::seconds(0)));
    cm::logic::TokenBucket bucket(kChunkSize, 0, makeClock(&fakeNow));

    std::uint64_t waitMs = 0;
    // 初始满桶仍可消费。
    EXPECT_EQ(cm::ErrorCode::Ok, bucket.tryAcquire(kChunkSize, &waitMs));
    // 桶空且速率为 0：永远无法补充 → 3006。
    EXPECT_EQ(cm::ErrorCode::RateLimited, bucket.tryAcquire(1, &waitMs));
}

TEST(RateLimiterTest, UserStreamLimit8) {
    cm::logic::StreamLimiter limiter(512, 8);
    const std::uint64_t uid = 1;

    for (int i = 0; i < 8; ++i) {
        EXPECT_TRUE(limiter.tryAcquire(uid));
    }
    EXPECT_FALSE(limiter.tryAcquire(uid));  // 第 9 个超限
    EXPECT_EQ(std::size_t(8), limiter.activeStreamsFor(uid));
    EXPECT_EQ(std::size_t(8), limiter.activeStreams());

    limiter.release(uid);
    EXPECT_TRUE(limiter.tryAcquire(uid));  // 释放后第 9 个成功
}

TEST(RateLimiterTest, GlobalStreamLimit512) {
    cm::logic::StreamLimiter limiter(512, 8);

    for (std::uint64_t uid = 0; uid < 512; ++uid) {
        EXPECT_TRUE(limiter.tryAcquire(uid));
    }
    EXPECT_FALSE(limiter.tryAcquire(999));  // 第 513 个全局超限
    EXPECT_EQ(std::size_t(512), limiter.activeStreams());
}

TEST(RateLimiterTest, StreamSlotReleasedOnClose) {
    cm::logic::StreamLimiter limiter(512, 8);
    const std::uint64_t uid = 42;

    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(limiter.tryAcquire(uid));
    }
    EXPECT_EQ(std::size_t(3), limiter.activeStreams());
    EXPECT_EQ(std::size_t(3), limiter.activeStreamsFor(uid));

    // 连接关闭：归还 3 个槽位。
    for (int i = 0; i < 3; ++i) {
        limiter.release(uid);
    }
    EXPECT_EQ(std::size_t(0), limiter.activeStreams());
    EXPECT_EQ(std::size_t(0), limiter.activeStreamsFor(uid));

    // 释放后可重新满额占用。
    for (int i = 0; i < 8; ++i) {
        EXPECT_TRUE(limiter.tryAcquire(uid));
    }
    EXPECT_FALSE(limiter.tryAcquire(uid));

    // 计划验收标准：重复 600 次 acquire/release 不泄漏计数（release 到 0 erase map 项、global 归零）。
    for (int i = 0; i < 8; ++i) {
        limiter.release(uid);
    }
    EXPECT_EQ(std::size_t(0), limiter.activeStreams());
    EXPECT_EQ(std::size_t(0), limiter.activeStreamsFor(uid));
    for (int i = 0; i < 600; ++i) {
        ASSERT_TRUE(limiter.tryAcquire(uid));
        limiter.release(uid);
    }
    EXPECT_EQ(std::size_t(0), limiter.activeStreams());
    EXPECT_EQ(std::size_t(0), limiter.activeStreamsFor(uid));
}

TEST(RateLimiterTest, SlowClientNoProgressClosed) {
    cm::logic::TimePoint fakeNow(cm::logic::TimePoint(std::chrono::seconds(0)));
    cm::logic::SlowClientGuard guard(makeClock(&fakeNow));

    // 无在途块：即使 30s 也不关闭（避免误杀正常空闲连接）。
    EXPECT_FALSE(guard.shouldClose());

    // 开始发块。
    guard.beginBlock();
    EXPECT_TRUE(guard.inFlight());
    EXPECT_FALSE(guard.shouldClose());

    // 29s 无进展：仍未超时。
    fakeNow += std::chrono::seconds(29);
    EXPECT_FALSE(guard.shouldClose());

    // 30s 无进展：应关闭。
    fakeNow += std::chrono::seconds(1);
    EXPECT_TRUE(guard.shouldClose());

    // 有进展则重置计时。
    guard.progress();
    fakeNow += std::chrono::seconds(29);
    EXPECT_FALSE(guard.shouldClose());
    fakeNow += std::chrono::seconds(1);
    EXPECT_TRUE(guard.shouldClose());

    // 块完成回到空闲：不再关闭。
    guard.endBlock();
    EXPECT_FALSE(guard.inFlight());
    EXPECT_FALSE(guard.shouldClose());
}

}  // namespace
