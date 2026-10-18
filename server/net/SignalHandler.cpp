#include "SignalHandler.h"

#include <cerrno>
#include <cstring>

#include "Channel.h"
#include "EventLoop.h"
#include "cm/memory.h"

#if defined(__linux__)
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace net {

int SignalHandler::sWriteFd = -1;

SignalHandler::SignalHandler(EventLoop* loop) : loop_(loop), received_(0) {
  pipefd_[0] = -1;
  pipefd_[1] = -1;
}

SignalHandler::~SignalHandler() {
#if defined(__linux__)
  for (size_t i = 0; i < signals_.size(); ++i) {
    ::signal(signals_[i], SIG_DFL);
  }
  if (channel_) {
    channel_->disableAll();
    channel_->remove();
    channel_.reset();
  }
  if (pipefd_[0] >= 0) {
    ::close(pipefd_[0]);
    pipefd_[0] = -1;
  }
  if (pipefd_[1] >= 0) {
    ::close(pipefd_[1]);
    pipefd_[1] = -1;
  }
  sWriteFd = -1;
#endif
}

#if defined(__linux__)

// 异步信号上下文：只允许 async-signal-safe 调用。
// write 是安全的；日志、printf、malloc、加锁都不是。
void SignalHandler::onSignal(int sig) {
  const int saved = errno;
  const unsigned char byte = static_cast<unsigned char>(sig & 0xFF);
  const ssize_t n = ::write(sWriteFd, &byte, 1);
  (void)n;
  errno = saved;
}

bool SignalHandler::install(const std::vector<int>& signals) {
  if (pipefd_[0] >= 0) {
    return true;
  }
  if (::pipe2(pipefd_, O_NONBLOCK | O_CLOEXEC) < 0) {
    return false;
  }
  sWriteFd = pipefd_[1];

  channel_ = cm::makeUnique<Channel>(loop_, pipefd_[0]);
  channel_->setReadCallback(std::bind(&SignalHandler::handleRead, this));
  channel_->enableReading();

  struct sigaction sa;
  std::memset(&sa, 0, sizeof sa);
  sa.sa_handler = &SignalHandler::onSignal;
  ::sigemptyset(&sa.sa_mask);
  // 不加 SA_RESTART：让阻塞中的 epoll_wait 能被 EINTR 打断，
  // 真正的唤醒源是管道可读事件
  sa.sa_flags = 0;

  for (size_t i = 0; i < signals.size(); ++i) {
    if (::sigaction(signals[i], &sa, nullptr) < 0) {
      return false;
    }
    signals_.push_back(signals[i]);
  }
  return true;
}

void SignalHandler::handleRead() {
  unsigned char buf[64];
  for (;;) {
    const ssize_t n = ::read(pipefd_[0], buf, sizeof buf);
    if (n > 0) {
      for (ssize_t i = 0; i < n; ++i) {
        ++received_;
        if (received_ >= 2) {
          ::_exit(1);  // 第二次信号：强制退出，避免卡住时无法结束进程
        }
        if (callback_) {
          callback_(static_cast<int>(buf[i]));
        }
      }
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  // ET + EPOLLONESHOT：读空后必须重新武装
  channel_->enableReading();
}

#else  // !__linux__

void SignalHandler::onSignal(int) {}

bool SignalHandler::install(const std::vector<int>& signals) {
  (void)signals;
  return false;
}

void SignalHandler::handleRead() {}

#endif  // __linux__

}  // namespace net
