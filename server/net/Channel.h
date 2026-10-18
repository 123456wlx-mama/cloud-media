#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "Poller.h"

namespace net {

class EventLoop;

// 一个 fd 的事件处理器：持有 fd 的引用与关注的事件，不拥有 fd。
// 所有 enable/disable 都会立即同步到 Poller（EPOLL_CTL_MOD）。
class Channel {
 public:
  Channel(EventLoop* loop, int fd);
  ~Channel();

  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;

  void handleEvent();

  void setReadCallback(const std::function<void()>& cb) { readCallback_ = cb; }
  void setWriteCallback(const std::function<void()>& cb) { writeCallback_ = cb; }
  void setErrorCallback(const std::function<void()>& cb) { errorCallback_ = cb; }
  void setCloseCallback(const std::function<void()>& cb) { closeCallback_ = cb; }

  void enableReading();
  void disableReading();
  void enableWriting();
  void disableWriting();
  void disableAll();

  bool isNoneEvent() const { return events_ == kNoneEvent; }
  bool isReading() const { return (events_ & kReadEvent) != 0; }
  bool isWriting() const { return (events_ & kWriteEvent) != 0; }

  int fd() const { return fd_; }
  uint32_t events() const { return events_; }
  void setRevents(uint32_t revents) { revents_ = revents; }

  // 从 Poller 摘除，禁止再收到事件
  void remove();

  // 绑定所有者（通常传 shared_from_this()）。
  // 事件回调期间提升成功即保证对象存活，避免 use-after-free。
  void tie(const std::shared_ptr<void>& owner);

  EventLoop* ownerLoop() const { return loop_; }

 private:
  void update();

  EventLoop* loop_;
  const int fd_;
  uint32_t events_;
  uint32_t revents_;
  bool tied_;
  std::weak_ptr<void> tie_;
  bool eventHandling_;
  bool removed_;

  std::function<void()> readCallback_;
  std::function<void()> writeCallback_;
  std::function<void()> errorCallback_;
  std::function<void()> closeCallback_;
};

}  // namespace net
