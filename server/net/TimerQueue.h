#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>

#include "Callbacks.h"

#if defined(__linux__)
#include <time.h>
#else
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

namespace net {

class EventLoop;
class Channel;

// 定时器句柄：拷贝安全，用序号校验，防止「已取消的槽位被复用后误删」
class TimerId {
 public:
  TimerId() : sequence_(0) {}
  bool valid() const { return sequence_ != 0; }
  int64_t sequence() const { return sequence_; }

 private:
  friend class TimerQueue;
  explicit TimerId(int64_t seq) : sequence_(seq) {}
  int64_t sequence_;
};

// 最小堆（std::set 按 when+seq 排序）+ timerfd 唤醒。
// Linux 用 timerfd 接入 epoll；其它平台用条件变量 + 后台线程模拟。
class TimerQueue {
 public:
  explicit TimerQueue(EventLoop* loop);
  ~TimerQueue();

  TimerQueue(const TimerQueue&) = delete;
  TimerQueue& operator=(const TimerQueue&) = delete;

  TimerId runAt(Timestamp when, TimerCallback cb);
  TimerId runAfter(int64_t delayMs, TimerCallback cb);
  TimerId runEvery(int64_t intervalMs, TimerCallback cb);
  void cancel(TimerId id);

  size_t size() const { return timers_.size(); }

 private:
  struct Entry {
    Timestamp when;
    int64_t sequence;
    int64_t intervalMs;  // > 0 表示周期定时器
    TimerCallback callback;
  };

  struct EntryLess {
    bool operator()(const Entry& lhs, const Entry& rhs) const {
      if (lhs.when != rhs.when) {
        return lhs.when < rhs.when;
      }
      return lhs.sequence < rhs.sequence;
    }
  };

  typedef std::set<Entry, EntryLess> TimerSet;

  TimerId addTimer(Timestamp when, int64_t intervalMs, TimerCallback cb);
  void processExpired();
  void armWakeup();
  void handleWakeup();

#if !defined(__linux__)
  void waiterLoop();
#endif

  EventLoop* loop_;
  TimerSet timers_;
  std::map<int64_t, Timestamp> whenOf_;  // seq -> when，用于 O(log n) 取消
  int64_t nextSequence_;

#if defined(__linux__)
  struct timespec toAbsolute(Timestamp when) const;

  int timerfd_;
  std::unique_ptr<Channel> timerChannel_;
  Timestamp steadyBase_;
  int64_t monoBaseMs_;
#else
  std::atomic<int64_t> nextWakeMs_;
  std::thread waiter_;
  std::mutex waitMutex_;
  std::condition_variable waitCond_;
  bool stopping_;
#endif
};

}  // namespace net
