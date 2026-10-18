#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "Callbacks.h"

namespace net {

class Channel;
class EventLoop;

// Channel 关注的事件用平台无关的位掩码表示，
// 由具体 Poller（epoll/poll/空实现）负责翻译成内核事件。
enum EventFlag : uint32_t {
  kNoneEvent = 0x00,
  kReadEvent = 0x01,
  kWriteEvent = 0x02,
  kErrorEvent = 0x04,
  kHupEvent = 0x08
};

class Poller {
 public:
  explicit Poller(EventLoop* loop);
  virtual ~Poller();

  Poller(const Poller&) = delete;
  Poller& operator=(const Poller&) = delete;

  // 等待事件，超时返回；返回当前时间戳（供上层统计/定时器使用）
  virtual Timestamp poll(int timeoutMs, std::vector<Channel*>* activeChannels) = 0;
  virtual void updateChannel(Channel* channel) = 0;
  virtual void removeChannel(Channel* channel) = 0;

  bool hasChannel(Channel* channel) const;

  // 工厂：Linux 返回 EpollPoller，其它平台返回编译占位实现
  static std::unique_ptr<Poller> createDefaultPoller(EventLoop* loop);

 protected:
  using ChannelMap = std::map<int, Channel*>;

  EventLoop* ownerLoop_;
  ChannelMap channels_;
};

}  // namespace net
