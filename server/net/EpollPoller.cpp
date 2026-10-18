#include "EpollPoller.h"

#include <cerrno>
#include <cstdlib>
#include <thread>

#include "Channel.h"

#if defined(__linux__)
#include <unistd.h>
#endif

namespace net {

#if defined(__linux__)

namespace {

// ET + ONESHOT 是全局策略：任何 Channel 注册进 epoll 都带这两个标志。
// 读事件必须一次读到 EAGAIN，处理完再 MOD 重新武装（见 Channel 的读回调约束）。
uint32_t toEpollEvents(uint32_t events) {
  uint32_t e = EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
  if (events & kReadEvent) {
    e |= (EPOLLIN | EPOLLPRI);
  }
  if (events & kWriteEvent) {
    e |= EPOLLOUT;
  }
  return e;
}

uint32_t fromEpollEvents(uint32_t events) {
  uint32_t r = kNoneEvent;
  // EPOLLRDHUP 归入读事件：对端半关闭时让读回调感知 read()==0
  if (events & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
    r |= kReadEvent;
  }
  if (events & EPOLLOUT) {
    r |= kWriteEvent;
  }
  if (events & EPOLLERR) {
    r |= kErrorEvent;
  }
  if (events & EPOLLHUP) {
    r |= kHupEvent;
  }
  return r;
}

}  // namespace

EpollPoller::EpollPoller(EventLoop* loop)
    : Poller(loop),
      epollfd_(::epoll_create1(EPOLL_CLOEXEC)),
      events_(kInitEventListSize) {
  if (epollfd_ < 0) {
    std::abort();
  }
}

EpollPoller::~EpollPoller() {
  if (epollfd_ >= 0) {
    ::close(epollfd_);
    epollfd_ = -1;
  }
}

Timestamp EpollPoller::poll(int timeoutMs, std::vector<Channel*>* activeChannels) {
  const int numEvents = ::epoll_wait(epollfd_, &*events_.begin(),
                                     static_cast<int>(events_.size()), timeoutMs);
  const int savedErrno = errno;
  const Timestamp now = std::chrono::steady_clock::now();

  if (numEvents > 0) {
    fillActiveChannels(numEvents, activeChannels);
    if (static_cast<size_t>(numEvents) == events_.size()) {
      events_.resize(events_.size() * 2);
    }
  } else if (numEvents < 0) {
    // EINTR 由调用方继续循环；其它错误保留 errno 供上层记录
    if (savedErrno != EINTR) {
      errno = savedErrno;
    }
  }
  return now;
}

void EpollPoller::fillActiveChannels(int numEvents,
                                    std::vector<Channel*>* activeChannels) const {
  for (int i = 0; i < numEvents; ++i) {
    Channel* channel = static_cast<Channel*>(events_[i].data.ptr);
    channel->setRevents(fromEpollEvents(events_[i].events));
    activeChannels->push_back(channel);
  }
}

void EpollPoller::updateChannel(Channel* channel) {
  const int fd = channel->fd();
  const uint32_t events = channel->events();
  ChannelMap::iterator it = channels_.find(fd);

  if (it == channels_.end()) {
    if (events == kNoneEvent) {
      return;
    }
    struct epoll_event ev = {};
    ev.events = toEpollEvents(events);
    ev.data.ptr = channel;
    if (::epoll_ctl(epollfd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
      return;
    }
    channels_[fd] = channel;
    return;
  }

  if (events == kNoneEvent) {
    ::epoll_ctl(epollfd_, EPOLL_CTL_DEL, fd, nullptr);
    channels_.erase(it);
    return;
  }

  struct epoll_event ev = {};
  ev.events = toEpollEvents(events);
  ev.data.ptr = channel;
  ::epoll_ctl(epollfd_, EPOLL_CTL_MOD, fd, &ev);
}

void EpollPoller::removeChannel(Channel* channel) {
  ChannelMap::iterator it = channels_.find(channel->fd());
  if (it == channels_.end()) {
    return;
  }
  ::epoll_ctl(epollfd_, EPOLL_CTL_DEL, channel->fd(), nullptr);
  channels_.erase(it);
}

#else  // !__linux__ —— Windows / 其它平台：编译占位空实现

EpollPoller::EpollPoller(EventLoop* loop) : Poller(loop) {}

EpollPoller::~EpollPoller() {}

Timestamp EpollPoller::poll(int timeoutMs, std::vector<Channel*>* activeChannels) {
  (void)activeChannels;
  if (timeoutMs < 0 || timeoutMs > 10) {
    timeoutMs = 10;  // 短轮询兜底，保证 quit()/queueInLoop 能被及时观察到
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
  return std::chrono::steady_clock::now();
}

void EpollPoller::updateChannel(Channel* channel) {
  channels_[channel->fd()] = channel;
}

void EpollPoller::removeChannel(Channel* channel) {
  channels_.erase(channel->fd());
}

#endif  // __linux__

}  // namespace net
