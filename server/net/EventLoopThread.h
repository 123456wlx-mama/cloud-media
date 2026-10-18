#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace net {

class EventLoop;

// 把 EventLoop 与一个 std::thread 绑定；startLoop() 返回时 loop 已就绪。
class EventLoopThread {
 public:
  explicit EventLoopThread(const std::string& name = std::string());
  ~EventLoopThread();

  EventLoopThread(const EventLoopThread&) = delete;
  EventLoopThread& operator=(const EventLoopThread&) = delete;

  EventLoop* startLoop();

 private:
  void threadFunc();

  EventLoop* loop_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cond_;
  std::string name_;
};

}  // namespace net
