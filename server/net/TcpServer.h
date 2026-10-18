#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "Callbacks.h"
#include "TcpConnection.h"

namespace net {

class EventLoop;
class Acceptor;

// 单 loop 的 TcpServer：accept 后在同一条 loop 上创建连接。
// 多 sub Reactor（EventLoopThreadPool）留到连接分配真正需要时再引入。
class TcpServer {
 public:
  TcpServer(EventLoop* loop, const std::string& name);
  ~TcpServer();

  TcpServer(const TcpServer&) = delete;
  TcpServer& operator=(const TcpServer&) = delete;

  // port 传 0 表示由内核分配，可通过 port() 取回
  bool start(uint16_t port);

  // 只停 accept，已有连接继续服务（优雅退出的第一步）
  void stopAccepting();
  // 对全部连接发起优雅 shutdown（发 FIN，等在途数据写完）
  void shutdownAll();
  // 停 accept + 立即关闭全部连接
  void stop();

  uint16_t port() const;

  void setMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }
  void setConnectionCallback(const std::function<void(const ConnectionPtr&)>& cb) {
    connectionCallback_ = cb;
  }

  size_t connectionCount() const { return connections_.size(); }

 private:
  void newConnection(int sockfd, const std::string& peer);
  void removeConnection(const ConnectionPtr& conn);
  void removeConnectionInLoop(const ConnectionPtr& conn);

  EventLoop* loop_;
  std::string name_;
  std::unique_ptr<Acceptor> acceptor_;
  std::map<std::string, ConnectionPtr> connections_;
  MessageCallback messageCallback_;
  std::function<void(const ConnectionPtr&)> connectionCallback_;
  int nextConnId_;
};

}  // namespace net
