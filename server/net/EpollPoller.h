#pragma once

#include <vector>

#include "Poller.h"

#if defined(__linux__)
#include <sys/epoll.h>
#endif

namespace net {

// Linux：epoll ET + EPOLLONESHOT。
// 其它平台：仅提供编译占位的空实现，poll() 退化为短睡眠轮询，保证 CI 能编译。
class EpollPoller : public Poller {
 public:
  explicit EpollPoller(EventLoop* loop);
  ~EpollPoller() override;

  Timestamp poll(int timeoutMs, std::vector<Channel*>* activeChannels) override;
  void updateChannel(Channel* channel) override;
  void removeChannel(Channel* channel) override;

 private:
#if defined(__linux__)
  enum { kInitEventListSize = 16 };

  void fillActiveChannels(int numEvents, std::vector<Channel*>* activeChannels) const;

  int epollfd_;
  std::vector<struct epoll_event> events_;
#endif
};

}  // namespace net
