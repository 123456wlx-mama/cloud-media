#include "TcpConnection.h"

#include <cerrno>
#include <chrono>
#include <cstdio>

#include "EventLoop.h"
#include "Socket.h"
#include "cm/memory.h"

#if defined(__linux__)
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace net {

namespace {

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

TcpConnection::TcpConnection(EventLoop* loop, const std::string& name, int sockfd,
                             const std::string& peerAddress)
    : loop_(loop),
      name_(name),
      peerAddress_(peerAddress),
      state_(kConnecting),
      socket_(cm::makeUnique<Socket>(sockfd)),
      channel_(cm::makeUnique<Channel>(loop, sockfd)),
      highWaterMark_(8 * 1024 * 1024),
      backpressure_(false),
      lastActiveMs_(nowMs()) {
  channel_->setReadCallback(std::bind(&TcpConnection::handleRead, this));
  channel_->setWriteCallback(std::bind(&TcpConnection::handleWrite, this));
  channel_->setErrorCallback(std::bind(&TcpConnection::handleError, this));
  channel_->setCloseCallback(std::bind(&TcpConnection::handleClose, this));
}

TcpConnection::~TcpConnection() {
  // socket_ 析构时关闭 fd；此时 Channel 必须已从 Poller 摘除
}

void TcpConnection::touchActive() { lastActiveMs_.store(nowMs()); }

void TcpConnection::connectEstablished() {
  if (state_.load() != kConnecting) {
    return;
  }
  state_.store(kConnected);
  // 关键：事件回调期间用 weak_ptr 提升保活，避免回调里被析构
  channel_->tie(shared_from_this());
  channel_->enableReading();
  if (connectionCallback_) {
    connectionCallback_(shared_from_this());
  }
}

void TcpConnection::connectDestroyed() {
  if (state_.load() == kConnected) {
    state_.store(kDisconnected);
    channel_->disableAll();
  }
  channel_->remove();
}

void TcpConnection::handleRead() {
  int savedErrno = 0;
  const std::ptrdiff_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);

  if (n > 0) {
    touchActive();
    if (messageCallback_) {
      messageCallback_(shared_from_this(), &inputBuffer_, std::chrono::steady_clock::now());
    }
  } else if (savedErrno == 0) {
    // read 返回 0：对端 FIN
    handleClose();
    return;
  } else if (savedErrno != EAGAIN && savedErrno != EWOULDBLOCK && savedErrno != EINTR) {
    handleError();
    return;
  }

  rearm();
}

void TcpConnection::handleWrite() {
#if defined(__linux__)
  if (!channel_->isWriting()) {
    rearm();  // 早返回也要重新武装读，避免“只写不收”
    return;
  }
  if (outputBuffer_.readableBytes() > 0) {
    const ssize_t n = ::send(channel_->fd(), outputBuffer_.peek(),
                             outputBuffer_.readableBytes(), MSG_NOSIGNAL);
    if (n > 0) {
      outputBuffer_.retrieve(static_cast<size_t>(n));
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      handleError();
      return;
    }
  }

  if (outputBuffer_.readableBytes() == 0) {
    // 写完立刻关掉 EPOLLOUT，避免空转把 CPU 打满
    channel_->disableWriting();
    if (backpressure_) {
      backpressure_ = false;
    }
    if (state_.load() == kDisconnecting) {
      shutdownInLoop();
      return;
    }
  }
#endif
  rearm();
}

void TcpConnection::handleClose() {
  if (state_.load() == kDisconnected) {
    return;
  }
  state_.store(kDisconnected);
  channel_->disableAll();
  channel_->remove();

  ConnectionPtr guard = shared_from_this();
  // 必须先以 connected()==false 触发 connectionCallback（上层靠它感知断开），
  // 再走 closeCallback 交由 TcpServer 回收；否则上层永远收不到断开通知。
  if (connectionCallback_) {
    connectionCallback_(guard);
  }
  if (closeCallback_) {
    closeCallback_(guard);
  }
}

void TcpConnection::handleError() { handleClose(); }

void TcpConnection::send(const std::string& message) {
  if (state_.load() == kDisconnected) {
    return;
  }
  if (loop_->isInLoopThread()) {
    sendInLoop(message);
    return;
  }
  // 跨线程：持 shared_ptr 入队，保证执行时对象仍存活
  ConnectionPtr self = shared_from_this();
  loop_->runInLoop(std::bind(&TcpConnection::sendInLoop, self, message));
}

void TcpConnection::send(const void* data, size_t len) {
  send(std::string(static_cast<const char*>(data), len));
}

void TcpConnection::sendInLoop(const std::string& message) {
#if defined(__linux__)
  if (state_.load() == kDisconnected) {
    return;
  }
  size_t written = 0;

  // 输出缓冲为空且没在等 EPOLLOUT：先尝试直接写
  if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
    const ssize_t n = ::send(channel_->fd(), message.data(), message.size(), MSG_NOSIGNAL);
    if (n > 0) {
      written = static_cast<size_t>(n);
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      handleError();
      return;
    }
  }

  if (written < message.size()) {
    outputBuffer_.append(message.data() + written, message.size() - written);
    if (!channel_->isWriting()) {
      channel_->enableWriting();
    }
    // 背压：发不出去就先别再收，等写空再恢复
    if (outputBuffer_.readableBytes() > highWaterMark_ && channel_->isReading()) {
      backpressure_ = true;
      channel_->disableReading();
    }
  }
#endif
}

void TcpConnection::rearm() {
  if (state_.load() != kConnected) {
    return;
  }
  if (backpressure_) {
    return;  // 输出积压期间不读，避免内存继续膨胀
  }
  channel_->enableReading();
}

void TcpConnection::shutdown() {
  if (state_.load() == kConnected || state_.load() == kConnecting) {
    state_.store(kDisconnecting);
    ConnectionPtr self = shared_from_this();
    loop_->runInLoop(std::bind(&TcpConnection::shutdownInLoop, self));
  }
}

void TcpConnection::shutdownInLoop() {
  channel_->disableReading();
  if (outputBuffer_.readableBytes() == 0) {
    socket_->shutdownWrite();  // 发 FIN，等对端回 FIN 后走 handleClose
  }
}

void TcpConnection::forceClose() {
  if (state_.load() == kDisconnected) {
    return;
  }
  ConnectionPtr self = shared_from_this();
  loop_->runInLoop(std::bind(&TcpConnection::forceCloseInLoop, self));
}

void TcpConnection::forceCloseInLoop() {
  if (state_.load() == kDisconnected) {
    return;
  }
  handleClose();
}

}  // namespace net
