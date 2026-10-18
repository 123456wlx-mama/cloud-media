#include "TimerQueue.h"

#include <cerrno>
#include <chrono>
#include <vector>

#include "EventLoop.h"
#include "cm/memory.h"

#if defined(__linux__)
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#endif

namespace net {

#if defined(__linux__)

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop), nextSequence_(0), timerfd_(-1), steadyBase_(std::chrono::steady_clock::now()) {
  struct timespec ts;
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  monoBaseMs_ = static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;

  timerfd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (timerfd_ >= 0) {
    timerChannel_ = cm::makeUnique<Channel>(loop_, timerfd_);
    timerChannel_->setReadCallback(std::bind(&TimerQueue::handleWakeup, this));
    timerChannel_->enableReading();
  }
}

TimerQueue::~TimerQueue() {
  if (timerChannel_) {
    timerChannel_->disableAll();
    timerChannel_->remove();
    timerChannel_.reset();
  }
  if (timerfd_ >= 0) {
    ::close(timerfd_);
    timerfd_ = -1;
  }
}

// steady_clock 的 epoch 与 CLOCK_MONOTONIC 不保证一致，
// 因此在构造时记录一次两个时钟的对应点，之后只做相对偏移换算。
struct timespec TimerQueue::toAbsolute(Timestamp when) const {
  const int64_t offsetMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               when - steadyBase_)
                               .count();
  int64_t absMs = monoBaseMs_ + offsetMs;
  if (absMs < monoBaseMs_) {
    absMs = monoBaseMs_;  // 已过期：立即触发
  }
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(absMs / 1000);
  ts.tv_nsec = static_cast<long>((absMs % 1000) * 1000000);
  return ts;
}

void TimerQueue::armWakeup() {
  if (timerfd_ < 0) {
    return;
  }
  struct itimerspec spec;
  spec.it_interval.tv_sec = 0;
  spec.it_interval.tv_nsec = 0;

  if (timers_.empty()) {
    spec.it_value.tv_sec = 0;
    spec.it_value.tv_nsec = 0;
    ::timerfd_settime(timerfd_, 0, &spec, nullptr);
    return;
  }
  spec.it_value = toAbsolute(timers_.begin()->when);
  ::timerfd_settime(timerfd_, TFD_TIMER_ABSTIME, &spec, nullptr);
}

void TimerQueue::handleWakeup() {
  uint64_t expirations = 0;
  while (::read(timerfd_, &expirations, sizeof expirations) > 0) {
  }
  processExpired();
  // ET + EPOLLONESHOT：处理完必须重新武装
  timerChannel_->enableReading();
}

#else  // !__linux__ —— 条件变量模拟

namespace {

int64_t steadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop), nextSequence_(0), nextWakeMs_(-1), stopping_(false) {
  waiter_ = std::thread(&TimerQueue::waiterLoop, this);
}

TimerQueue::~TimerQueue() {
  stopping_ = true;
  waitCond_.notify_all();
  if (waiter_.joinable()) {
    waiter_.join();
  }
}

void TimerQueue::armWakeup() {
  if (timers_.empty()) {
    nextWakeMs_.store(-1);
  } else {
    nextWakeMs_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                          timers_.begin()->when.time_since_epoch())
                          .count());
  }
  waitCond_.notify_all();
}

void TimerQueue::waiterLoop() {
  while (!stopping_) {
    {
      std::unique_lock<std::mutex> lock(waitMutex_);
      waitCond_.wait_for(lock, std::chrono::milliseconds(10));
    }
    if (stopping_) {
      break;
    }
    const int64_t target = nextWakeMs_.load();
    if (target >= 0 && steadyNowMs() >= target) {
      nextWakeMs_.store(-1);
      loop_->runInLoop(std::bind(&TimerQueue::processExpired, this));
    }
  }
}

#endif  // __linux__

TimerId TimerQueue::addTimer(Timestamp when, int64_t intervalMs, TimerCallback cb) {
  loop_->assertInLoopThread();

  Entry entry;
  entry.when = when;
  entry.sequence = ++nextSequence_;
  entry.intervalMs = intervalMs;
  entry.callback = cb;

  timers_.insert(entry);
  whenOf_[entry.sequence] = entry.when;
  armWakeup();
  return TimerId(entry.sequence);
}

TimerId TimerQueue::runAt(Timestamp when, TimerCallback cb) {
  return addTimer(when, 0, cb);
}

TimerId TimerQueue::runAfter(int64_t delayMs, TimerCallback cb) {
  if (delayMs < 0) {
    delayMs = 0;
  }
  return addTimer(std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs), 0, cb);
}

TimerId TimerQueue::runEvery(int64_t intervalMs, TimerCallback cb) {
  if (intervalMs <= 0) {
    intervalMs = 1;
  }
  return addTimer(std::chrono::steady_clock::now() + std::chrono::milliseconds(intervalMs),
                  intervalMs, cb);
}

void TimerQueue::cancel(TimerId id) {
  // 与 addTimer 同侧的线程约束：timers_/whenOf_ 只在 loop 线程读写
  loop_->assertInLoopThread();
  if (!id.valid()) {
    return;
  }
  std::map<int64_t, Timestamp>::iterator it = whenOf_.find(id.sequence());
  if (it == whenOf_.end()) {
    return;  // 已触发或已取消，无副作用
  }
  const Timestamp when = it->second;
  whenOf_.erase(it);

  Entry key;
  key.when = when;
  key.sequence = id.sequence();
  key.intervalMs = 0;
  timers_.erase(key);

  armWakeup();
}

void TimerQueue::processExpired() {
  loop_->assertInLoopThread();

  const Timestamp now = std::chrono::steady_clock::now();
  std::vector<Entry> expired;

  TimerSet::iterator it = timers_.begin();
  while (it != timers_.end() && it->when <= now) {
    expired.push_back(*it);
    whenOf_.erase(it->sequence);
    it = timers_.erase(it);
  }

  // 先全部摘除再执行：允许回调里取消其它定时器或新增定时器
  for (size_t i = 0; i < expired.size(); ++i) {
    Entry& entry = expired[i];
    if (entry.callback) {
      entry.callback();
    }
    if (entry.intervalMs > 0) {
      Timestamp next = entry.when + std::chrono::milliseconds(entry.intervalMs);
      if (next <= now) {
        next = now + std::chrono::milliseconds(entry.intervalMs);  // 落后太多则跳过丢掉的 tick
      }
      addTimer(next, entry.intervalMs, entry.callback);
    }
  }

  armWakeup();
}

}  // namespace net
