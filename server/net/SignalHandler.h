#pragma once

#include <functional>
#include <memory>
#include <vector>

namespace net {

class EventLoop;
class Channel;

// self-pipe 信号处理器：handler 内只做一次 async-signal-safe 的 write，
// 真正的处理在 loop 线程里完成。第二次信号直接 _exit(1)，用于强制退出。
class SignalHandler {
 public:
  explicit SignalHandler(EventLoop* loop);
  ~SignalHandler();

  SignalHandler(const SignalHandler&) = delete;
  SignalHandler& operator=(const SignalHandler&) = delete;

  void setCallback(const std::function<void(int)>& cb) { callback_ = cb; }
  bool install(const std::vector<int>& signals);

 private:
  void handleRead();

  EventLoop* loop_;
  int pipefd_[2];
  std::unique_ptr<Channel> channel_;
  std::function<void(int)> callback_;
  std::vector<int> signals_;
  int received_;

  static int sWriteFd;
  static void onSignal(int sig);
};

}  // namespace net
