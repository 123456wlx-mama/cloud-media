#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

#include "EventLoop.h"

namespace {

using net::EventLoop;

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

// 1) 同线程投递必须同步执行（不能绕道队列）
TEST(EventLoopTest, RunInLoopSameThreadImmediate) {
  EventLoop loop;
  ASSERT_TRUE(loop.isInLoopThread());

  bool ran = false;
  loop.runInLoop([&ran]() { ran = true; });

  EXPECT_TRUE(ran);
}

// 2) 跨线程投递 1000 个任务，必须全部被执行（验证 eventfd 唤醒 + 队列 swap）
TEST(EventLoopTest, QueueInLoopFromOtherThread) {
  // EventLoop 必须在其所属线程内构造：loop() 会断言线程归属
  std::atomic<EventLoop*> loopPtr(nullptr);
  std::atomic<bool> ready(false);
  std::atomic<int> count(0);

  std::thread worker([&loopPtr, &ready]() {
    EventLoop loop;
    loopPtr.store(&loop);
    ready.store(true);
    loop.loop();
    loopPtr.store(nullptr);
  });

  ASSERT_TRUE(waitFor([&ready]() { return ready.load(); }, 1000));
  EventLoop* loop = loopPtr.load();
  ASSERT_NE(loop, nullptr);

  for (int i = 0; i < 1000; ++i) {
    loop->queueInLoop([&count]() { count.fetch_add(1); });
  }

  const bool allDone = waitFor([&count]() { return count.load() == 1000; }, 3000);

  loop->quit();
  worker.join();

  EXPECT_TRUE(allDone);
  EXPECT_EQ(count.load(), 1000);
}

// 3) loop 阻塞在 epoll_wait 时，quit() 必须立刻唤醒并退出（不能等超时）
TEST(EventLoopTest, QuitWakesUpBlockedPoll) {
  std::atomic<EventLoop*> loopPtr(nullptr);
  std::atomic<bool> ready(false);

  std::thread worker([&loopPtr, &ready]() {
    EventLoop loop;
    loopPtr.store(&loop);
    ready.store(true);
    loop.loop();
    loopPtr.store(nullptr);
  });

  const bool entered = waitFor([&ready]() { return ready.load(); }, 1000);
  EventLoop* loop = loopPtr.load();

  // 让 loop 真正阻塞在 poll 上，否则测不出「唤醒」而只是测到「未进入循环」
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
  if (loop != nullptr) {
    loop->quit();
  }
  worker.join();
  const long long elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - begin).count();

  EXPECT_TRUE(entered);
  EXPECT_NE(loop, nullptr);
  EXPECT_LT(elapsedMs, 200);
}
