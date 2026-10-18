#include "Channel.h"

#include <memory>

#include "EventLoop.h"

namespace net {

Channel::Channel(EventLoop* loop, int fd)
    : loop_(loop),
      fd_(fd),
      events_(kNoneEvent),
      revents_(kNoneEvent),
      tied_(false),
      eventHandling_(false),
      removed_(false) {}

Channel::~Channel() {
  if (!removed_) {
    remove();
  }
}

void Channel::tie(const std::shared_ptr<void>& owner) {
  tie_ = owner;
  tied_ = true;
}

void Channel::update() {
  loop_->updateChannel(this);
}

void Channel::remove() {
  if (!removed_) {
    loop_->removeChannel(this);
    removed_ = true;
  }
}

void Channel::enableReading() {
  events_ |= static_cast<uint32_t>(kReadEvent);
  update();
}

void Channel::disableReading() {
  events_ &= ~static_cast<uint32_t>(kReadEvent);
  update();
}

void Channel::enableWriting() {
  events_ |= static_cast<uint32_t>(kWriteEvent);
  update();
}

void Channel::disableWriting() {
  events_ &= ~static_cast<uint32_t>(kWriteEvent);
  update();
}

void Channel::disableAll() {
  events_ = kNoneEvent;
  update();
}

void Channel::handleEvent() {
  // 若绑定了所有者，事件处理期间延长其生命周期；提升失败说明对象正在销毁
  std::shared_ptr<void> guard;
  if (tied_) {
    guard = tie_.lock();
    if (!guard) {
      return;
    }
  }

  eventHandling_ = true;

  if (revents_ & kErrorEvent) {
    if (errorCallback_) {
      errorCallback_();
    }
  }

  if (revents_ & kHupEvent) {
    if (closeCallback_) {
      closeCallback_();
    }
  } else {
    if (revents_ & kReadEvent) {
      if (readCallback_) {
        readCallback_();
      }
    }
    if (revents_ & kWriteEvent) {
      if (writeCallback_) {
        writeCallback_();
      }
    }
  }

  eventHandling_ = false;
}

}  // namespace net
