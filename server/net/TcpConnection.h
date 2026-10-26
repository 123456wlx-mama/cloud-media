#pragma once

#include <atomic>
#include <cstddef>
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

  // 发送文件内容：连接接管 fd；先发完「文件帧头」，再 sendfile，最后才发期间新产生的响应。
  // done 只会被调用一次，且在本连接所属 loop 线程。
  bool sendFile(int fd, std::int64_t offset, std::size_t len,
                const std::function<void(bool)>& done);

  // 暂停/恢复读事件；readPaused_ 会阻止 rearm 在回调尾部重新打开 EPOLLIN。
  void pauseRead();
  void resumeRead();
  bool readPaused() const { return readPaused_.load(); }
  bool isReading() const { return channel_->isReading(); }
  bool isWriting() const { return channel_->isWriting(); }

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
  // 把 buf 里可读数据循环写到 socket，直到写空或 EAGAIN。
  // 返回 false 表示连接已关闭（内部已走 handleError），调用方必须立即返回。
  bool flushOutputBuffer(cm::Buffer& buf);
  void sendFileInLoop(int fd, std::int64_t offset, std::size_t len,
                      const std::function<void(bool)>& done);
  void startSendFileInLoop();
  bool sendFileLoop();
  void finishSendFile(bool ok);
  void cancelSendFile();
  void shutdownInLoop();
  void forceCloseInLoop();
  // ET + ONESHOT 的重新武装：每次事件回调结束都必须把「仍需关注的事件」重新 MOD 回去。
  // 待发数据优先恢复可写关注，背压只暂停读，不能连写一起停掉。
  void rearm();
  // 还有没写完的东西（普通响应 / 文件帧头 / 文件本体）
  bool hasPendingOutput() const;
  // 输出积压超过高水位：暂停读，等 handleWrite 写空后恢复。
  // 幂等；不要求当前正在读（readPaused 期间超水位同样要记下来）。
  void applyBackpressure();
  void touchActive();

  EventLoop* loop_;
  const std::string name_;
  const std::string peerAddress_;
  std::atomic<State> state_;

  std::unique_ptr<Socket> socket_;
  std::unique_ptr<Channel> channel_;

  cm::Buffer inputBuffer_;
  cm::Buffer outputBuffer_;

  // sendfile 的文件帧头单独存放：必须先于文件数据发出，且期间新到达的响应
  // 只能排在文件数据之后，绝不能混进这个缓冲。
  cm::Buffer sendFileHeader_;

  MessageCallback messageCallback_;
  std::function<void(const ConnectionPtr&)> connectionCallback_;
  CloseCallback closeCallback_;

  size_t highWaterMark_;
  bool backpressure_;
  std::atomic<bool> readPaused_;
  std::atomic<int64_t> lastActiveMs_;

  int sendFileFd_;
  std::int64_t sendFileOffset_;
  std::size_t sendFileRemaining_;
  bool sendFilePending_;
  bool sendFileActive_;
  std::function<void(bool)> sendFileDone_;
};

}  // namespace net
