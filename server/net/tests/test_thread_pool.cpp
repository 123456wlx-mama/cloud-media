#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

#include "cm/base/thread_pool.h"

namespace {

bool waitFor(const std::function<bool()>& pred, int timeoutMs) {
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return pred();
}

}  // namespace

// 1) 提交的任务必须全部执行，不能丢
TEST(ThreadPoolTest, RunsAllSubmittedTasks) {
  cm::ThreadPool pool(4, "test");
  std::atomic<int> count(0);

  for (int i = 0; i < 100; ++i) {
    EXPECT_TRUE(pool.run([&count]() { count.fetch_add(1); }));
  }

  EXPECT_TRUE(waitFor([&count]() { return count.load() == 100; }, 3000));
  EXPECT_EQ(count.load(), 100);
}

// 2) stop() 之后必须拒绝新任务（返回 false，且不执行）
TEST(ThreadPoolTest, RejectsTasksAfterStop) {
  cm::ThreadPool pool(2, "test");
  pool.stop();

  std::atomic<bool> ran(false);
  EXPECT_FALSE(pool.run([&ran]() { ran.store(true); }));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  EXPECT_FALSE(ran.load());
  EXPECT_EQ(pool.pendingTasks(), 0u);
}

// 3) 析构必须等待队列中的任务跑完，不能丢半截
TEST(ThreadPoolTest, DestructorDrainsPendingTasks) {
  std::atomic<int> count(0);
  {
    cm::ThreadPool pool(2, "test");
    for (int i = 0; i < 50; ++i) {
      pool.run([&count]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        count.fetch_add(1);
      });
    }
  }  // ~ThreadPool -> stop() -> join
  EXPECT_EQ(count.load(), 50);
}
