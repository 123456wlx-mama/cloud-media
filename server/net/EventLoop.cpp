#include "EventLoop.h"

#include <cassert>
#include <cstdint>
#include <utility>

#include "cm/memory.h"

#if defined(__linux__)
#include <cstdlib>
#include <unistd.h>
#include <sys/eventfd.h>
#endif

namespace net {

namespace {

#if defined(__linux__)
// Linux 下由 eventfd 唤醒，可以阻塞得久一些
const int kPollTimeoutMs = 10000;
#else
// 无 eventfd：靠短睡眠轮询兜底，保证 quit 能在 ~10ms 内生效
const int kPollTimeoutMs = 10;
#endif

}  // namespace

EventLoop::EventLoop()
    : quit_(false),
      looping_(false),
      callingPendingFunctors_(false),
      threadId_(std::this_thread::get_id()),
      poller_(Poller::createDefaultPoller(this)),
      wakeupFd_(-1) {
#if defined(__linux__)
  wakeupFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wakeupFd_ < 0) {
    std::abort();
  }
  wakeupChannel_ = cm::makeUnique<Channel>(this, wakeupFd_);
  wakeupChannel_->setReadCallback(std::bind(&EventLoop::handleWakeup, this));
  wakeupChannel_->enableReading();
#endif
}

EventLoop::~EventLoop() {
  assert(!looping_);
#if defined(__linux__)
  if (wakeupChannel_) {
    wakeupChannel_->disableAll();
    wakeupChannel_->remove();
  }
  if (wakeupFd_ >= 0) {
    ::close(wakeupFd_);
    wakeupFd_ = -1;
  }
#endif
}

void EventLoop::loop() {
  assertInLoopThread();
  looping_ = true;

  std::vector<Channel*> activeChannels;
  while (!quit_.load()) {
    activeChannels.clear();
    poller_->poll(kPollTimeoutMs, &activeChannels);
    for (std::vector<Channel*>::iterator it = activeChannels.begin();
         it != activeChannels.end(); ++it) {
      (*it)->handleEvent();
    }
    doPendingFunctors();
  }

  looping_ = false;
}

void EventLoop::quit() {
  quit_.store(true);
  // 不阻塞在 epoll_wait 里：直接唤醒
  if (!isInLoopThread()) {
    wakeup();
  }
}

void EventLoop::runInLoop(Functor cb) {
  if (isInLoopThread()) {
    cb();
  } else {
    queueInLoop(std::move(cb));
  }
}

void EventLoop::queueInLoop(Functor cb) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingFunctors_.push_back(std::move(cb));
  }
  // 正在执行回调时也要唤醒：本轮 doPendingFunctors 已经 swap 出了队列
  if (!isInLoopThread() || callingPendingFunctors_) {
    wakeup();
  }
}

void EventLoop::wakeup() {
#if defined(__linux__)
  if (wakeupFd_ < 0) {
    return;
  }
  const uint64_t one = 1;
  const ssize_t n = ::write(wakeupFd_, &one, sizeof one);
  (void)n;
#endif
}

void EventLoop::handleWakeup() {
#if defined(__linux__)
  // ET 要求一次读干净：一直读到 EAGAIN 为止
  uint64_t value = 0;
  while (::read(wakeupFd_, &value, sizeof value) > 0) {
  }
  // EPOLLONESHOT 触发后已被内核摘除，必须重新武装，否则下一轮再也不会被唤醒
  wakeupChannel_->enableReading();
#endif
}

void EventLoop::doPendingFunctors() {
  std::vector<Functor> functors;
  callingPendingFunctors_ = true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    functors.swap(pendingFunctors_);
  }
  for (size_t i = 0; i < functors.size(); ++i) {
    functors[i]();
  }
  callingPendingFunctors_ = false;
}

void EventLoop::assertInLoopThread() const {
  assert(isInLoopThread());
}

void EventLoop::updateChannel(Channel* channel) {
  assertInLoopThread();
  poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel) {
  assertInLoopThread();
  poller_->removeChannel(channel);
}

bool EventLoop::hasChannel(Channel* channel) const {
  return poller_->hasChannel(channel);
}

}  // namespace net
