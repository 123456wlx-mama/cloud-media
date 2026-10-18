#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "Callbacks.h"
#include "Channel.h"
#include "cm/base/buffer.h"

namespace net {

class EventLoop;
class Socket;

// 一条 TCP 连接：状态只允许在其所属 loop 线程内变更。
// 生命周期由 TcpServer 的连接表持有（shared_ptr），关闭时经由 queueInLoop 延迟析构。
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
 public:
  enum State { kConnecting, kConnected, kDisconnecting, kDisconnected };

  TcpConnection(EventLoop* loop, const std::string& name, int sockfd,
                const std::string& peerAddress);
  ~TcpConnection();

  TcpConnection(const TcpConnection&) = delete;
  TcpConnection& operator=(const TcpConnection&) = delete;

  EventLoop* getLoop() const { return loop_; }
  const std::string& name() const { return name_; }
  const std::string& peerAddress() const { return peerAddress_; }
  bool connected() const { return state_.load() == kConnected; }
  int64_t lastActiveMs() const { return lastActiveMs_.load(); }

  void send(const std::string& message);
  void send(const void* data, size_t len);

  // 优雅关闭：停读 -> 发完缓冲 -> SHUT_WR -> 等对端 FIN
  void shutdown();
  // 立即关闭（错误/超时/服务端退出）
  void forceClose();

  void setMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }
  void setConnectionCallback(const std::function<void(const ConnectionPtr&)>& cb) {
    connectionCallback_ = cb;
  }
  void setCloseCallback(const CloseCallback& cb) { closeCallback_ = cb; }

  void connectEstablished();
  void connectDestroyed();

 private:
  void handleRead();
  void handleWrite();
  void handleClose();
  void handleError();
  void sendInLoop(const std::string& message);
  void shutdownInLoop();
  void forceCloseInLoop();
  // ET + ONESHOT 的重新武装；不满足条件时什么都不做
  void rearm();
  void touchActive();

  EventLoop* loop_;
  const std::string name_;
  const std::string peerAddress_;
  std::atomic<State> state_;

  std::unique_ptr<Socket> socket_;
  std::unique_ptr<Channel> channel_;

  cm::Buffer inputBuffer_;
  cm::Buffer outputBuffer_;

  MessageCallback messageCallback_;
  std::function<void(const ConnectionPtr&)> connectionCallback_;
  CloseCallback closeCallback_;

  size_t highWaterMark_;
  bool backpressure_;
  std::atomic<int64_t> lastActiveMs_;
};

}  // namespace net
