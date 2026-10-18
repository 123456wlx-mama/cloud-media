#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "Callbacks.h"
#include "Channel.h"
#include "Poller.h"

namespace net {

// 单线程事件循环：一个 loop 绑定一个线程，所有 Channel 的状态变更都在该线程内完成。
// ET + EPOLLONESHOT 语义下，读回调必须读到 EAGAIN 并自行重新武装（见 handleWakeup）。
class EventLoop {
 public:
  EventLoop();
  ~EventLoop();

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  void loop();
  void quit();

  // 本线程内直接执行，否则入队并由 eventfd 唤醒
  void runInLoop(Functor cb);
  void queueInLoop(Functor cb);

  bool isInLoopThread() const { return threadId_ == std::this_thread::get_id(); }
  void assertInLoopThread() const;

  void updateChannel(Channel* channel);
  void removeChannel(Channel* channel);
  bool hasChannel(Channel* channel) const;

 private:
  void wakeup();
  void handleWakeup();
  void doPendingFunctors();

  std::atomic<bool> quit_;
  bool looping_;
  bool callingPendingFunctors_;
  std::thread::id threadId_;

  std::unique_ptr<Poller> poller_;

  int wakeupFd_;
  std::unique_ptr<Channel> wakeupChannel_;

  std::mutex mutex_;
  std::vector<Functor> pendingFunctors_;
};

}  // namespace net
