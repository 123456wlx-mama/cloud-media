#include "EventLoopThread.h"

#include "EventLoop.h"

namespace net {

EventLoopThread::EventLoopThread(const std::string& name) : loop_(nullptr), name_(name) {}

EventLoopThread::~EventLoopThread() {
  if (!thread_.joinable()) {
    return;
  }
  EventLoop* loop = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    loop = loop_;
  }
  if (loop != nullptr) {
    loop->quit();
  }
  thread_.join();
}

EventLoop* EventLoopThread::startLoop() {
  thread_ = std::thread(std::bind(&EventLoopThread::threadFunc, this));

  EventLoop* loop = nullptr;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    while (loop_ == nullptr) {
      cond_.wait(lock);
    }
    loop = loop_;
  }
  return loop;
}

void EventLoopThread::threadFunc() {
  EventLoop loop;  // 在子线程内构造：loop 的线程归属必须正确

  {
    std::lock_guard<std::mutex> lock(mutex_);
    loop_ = &loop;
    cond_.notify_one();
  }

  loop.loop();

  std::lock_guard<std::mutex> lock(mutex_);
  loop_ = nullptr;
}

}  // namespace net
