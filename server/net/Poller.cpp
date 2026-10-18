#include "Poller.h"

#include "Channel.h"
#include "EpollPoller.h"
#include "cm/memory.h"

namespace net {

Poller::Poller(EventLoop* loop) : ownerLoop_(loop) {}

Poller::~Poller() {}

bool Poller::hasChannel(Channel* channel) const {
  ChannelMap::const_iterator it = channels_.find(channel->fd());
  return it != channels_.end() && it->second == channel;
}

std::unique_ptr<Poller> Poller::createDefaultPoller(EventLoop* loop) {
  return cm::makeUnique<EpollPoller>(loop);
}

}  // namespace net
