#include "TcpServer.h"

#include <cstdio>

#include <vector>

#include "Acceptor.h"
#include "EventLoop.h"
#include "cm/memory.h"

namespace net {

TcpServer::TcpServer(EventLoop* loop, const std::string& name)
    : loop_(loop),
      name_(name),
      acceptor_(cm::makeUnique<Acceptor>(loop, name)),
      nextConnId_(0) {
  acceptor_->setNewConnectionCallback(
      std::bind(&TcpServer::newConnection, this, std::placeholders::_1, std::placeholders::_2));
}

TcpServer::~TcpServer() {
  acceptor_.reset();
  // 连接对象在此析构：socket 关闭（调用方需保证此时已不在 loop 事件中）
  connections_.clear();
}

bool TcpServer::start(uint16_t port) { return acceptor_->listen(port); }

uint16_t TcpServer::port() const { return acceptor_->port(); }

void TcpServer::stopAccepting() {
  if (acceptor_) {
    acceptor_->stop();
  }
}

namespace {

// 先拷贝再操作：close 回调会在同线程内同步 erase connections_，
// 直接遍历 map 会踩到迭代器失效
std::vector<ConnectionPtr> snapshot(const std::map<std::string, ConnectionPtr>& conns) {
  std::vector<ConnectionPtr> result;
  result.reserve(conns.size());
  for (std::map<std::string, ConnectionPtr>::const_iterator it = conns.begin();
       it != conns.end(); ++it) {
    result.push_back(it->second);
  }
  return result;
}

}  // namespace

void TcpServer::shutdownAll() {
  const std::vector<ConnectionPtr> conns = snapshot(connections_);
  for (size_t i = 0; i < conns.size(); ++i) {
    conns[i]->shutdown();
  }
}

void TcpServer::stop() {
  stopAccepting();
  const std::vector<ConnectionPtr> conns = snapshot(connections_);
  for (size_t i = 0; i < conns.size(); ++i) {
    conns[i]->forceClose();
  }
}

void TcpServer::newConnection(int sockfd, const std::string& peer) {
  char suffix[32];
  std::snprintf(suffix, sizeof suffix, "#%d", ++nextConnId_);
  const std::string connName = name_ + "-" + peer + suffix;

  ConnectionPtr conn = std::make_shared<TcpConnection>(loop_, connName, sockfd, peer);
  conn->setMessageCallback(messageCallback_);
  conn->setConnectionCallback(connectionCallback_);
  conn->setCloseCallback(std::bind(&TcpServer::removeConnection, this, std::placeholders::_1));

  connections_[connName] = conn;
  conn->connectEstablished();
}

void TcpServer::removeConnection(const ConnectionPtr& conn) {
  loop_->runInLoop(std::bind(&TcpServer::removeConnectionInLoop, this, conn));
}

void TcpServer::removeConnectionInLoop(const ConnectionPtr& conn) {
  connections_.erase(conn->name());
  // 延迟到下一轮再销毁：此刻仍在 handleEvent 的调用栈上
  loop_->queueInLoop(std::bind(&TcpConnection::connectDestroyed, conn));
}

}  // namespace net
