#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "Channel.h"
#include "Socket.h"

namespace net {

class EventLoop;

class Acceptor {
 public:
  using NewConnectionCallback = std::function<void(int sockfd, const std::string& peer)>;

  Acceptor(EventLoop* loop, const std::string& name);
  ~Acceptor();

  Acceptor(const Acceptor&) = delete;
  Acceptor& operator=(const Acceptor&) = delete;

  void setNewConnectionCallback(const NewConnectionCallback& cb);
  bool listen(uint16_t port);
  void stop();

  uint16_t port() const { return port_; }

 private:
  void handleRead();

  EventLoop* loop_;
  std::string name_;
  std::unique_ptr<Socket> acceptSocket_;
  std::unique_ptr<Channel> acceptChannel_;
  NewConnectionCallback newConnectionCallback_;
  uint16_t port_;
  bool listening_;
  // 预留 fd：fd 耗尽时先关它 → accept → 立即关掉新连接 → 再重新占回来，
  // 避免 EMFILE 时 accept 一直失败导致 epoll 空转。
  int idleFd_;
};

}  // namespace net
