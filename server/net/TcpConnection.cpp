#include "TcpConnection.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>

#include "EventLoop.h"
#include "Socket.h"
#include "cm/memory.h"

#if defined(__linux__)
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace net {

namespace {

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void closeFd(int fd) {
#if defined(__linux__)
  if (fd >= 0) {
    ::close(fd);
  }
#else
  (void)fd;
#endif
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
      readPaused_(false),
      lastActiveMs_(nowMs()),
      sendFileFd_(-1),
      sendFileOffset_(0),
      sendFileRemaining_(0),
      sendFilePending_(false),
      sendFileActive_(false) {
  channel_->setReadCallback(std::bind(&TcpConnection::handleRead, this));
  channel_->setWriteCallback(std::bind(&TcpConnection::handleWrite, this));
  channel_->setErrorCallback(std::bind(&TcpConnection::handleError, this));
  channel_->setCloseCallback(std::bind(&TcpConnection::handleClose, this));
}

TcpConnection::~TcpConnection() {
  // socket_ 析构时关闭 fd；此时 Channel 必须已从 Poller 摘除
  if (sendFileFd_ >= 0) {
    closeFd(sendFileFd_);
    sendFileFd_ = -1;
  }
}

void TcpConnection::touchActive() { lastActiveMs_.store(nowMs()); }

void TcpConnection::connectEstablished() {
  if (state_.load() != kConnecting) {
    return;
  }
  state_.store(kConnected);
  readPaused_.store(false);
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
  cancelSendFile();
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

  // 先把 outputBuffer（含帧头）写空，再启动 sendfile，保证协议字节序。
  if (sendFilePending_ && outputBuffer_.readableBytes() == 0) {
    startSendFileInLoop();
    if (state_.load() == kDisconnected) {
      return;
    }
  }

  if (sendFileActive_) {
    if (!sendFileLoop()) {
      if (state_.load() == kDisconnected) {
        return;
      }
      rearm();
      return;
    }
  }

  if (state_.load() == kDisconnected) {
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

  // 帧头刚写完：同一轮继续启动 sendfile。
  if (sendFilePending_ && outputBuffer_.readableBytes() == 0) {
    startSendFileInLoop();
    if (state_.load() == kDisconnected) {
      return;
    }
    if (sendFileActive_ && !sendFileLoop()) {
      if (state_.load() == kDisconnected) {
        return;
      }
      rearm();
      return;
    }
  }

  if (state_.load() == kDisconnected) {
    return;
  }

  if (outputBuffer_.readableBytes() == 0 && !sendFileActive_ &&
      !sendFilePending_) {
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
  cancelSendFile();

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

  // 大文件发送期间的新数据只能排到文件之后；不允许插到 payload 中间。
  if (sendFileActive_ || sendFilePending_) {
    if (!message.empty()) {
      outputBuffer_.append(message.data(), message.size());
      if (!channel_->isWriting()) {
        channel_->enableWriting();
      }
    }
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

bool TcpConnection::sendFile(int fd, std::int64_t offset, std::size_t len,
                               const std::function<void(bool)>& done) {
  if (fd < 0 || offset < 0) {
    loop_->runInLoop([fd, done]() {
      closeFd(fd);
      if (done) {
        done(false);
      }
    });
    return false;
  }
  ConnectionPtr self = shared_from_this();
  loop_->runInLoop([self, fd, offset, len, done]() {
    self->sendFileInLoop(fd, offset, len, done);
  });
  return true;
}

void TcpConnection::sendFileInLoop(int fd, std::int64_t offset,
                                   std::size_t len,
                                   const std::function<void(bool)>& done) {
  if (fd < 0 || offset < 0 || state_.load() != kConnected ||
      sendFileActive_ || sendFilePending_) {
    closeFd(fd);
    if (done) {
      done(false);
    }
    return;
  }
  if (len == 0) {
    closeFd(fd);
    if (done) {
      done(true);
    }
    return;
  }

  sendFileFd_ = fd;
  sendFileOffset_ = offset;
  sendFileRemaining_ = len;
  sendFileDone_ = done;
  sendFilePending_ = true;

  if (outputBuffer_.readableBytes() == 0) {
    startSendFileInLoop();
  } else if (!channel_->isWriting()) {
    channel_->enableWriting();
  }
}

void TcpConnection::startSendFileInLoop() {
  if (!sendFilePending_ || sendFileActive_) {
    return;
  }
  sendFilePending_ = false;
  sendFileActive_ = true;
  sendFileLoop();
}

bool TcpConnection::sendFileLoop() {
  if (!sendFileActive_) {
    return false;
  }
#if defined(__linux__)
  const std::size_t kSlice = 1024 * 1024;
  while (sendFileRemaining_ > 0) {
    const std::size_t chunk = std::min(sendFileRemaining_, kSlice);
    off_t offset = static_cast<off_t>(sendFileOffset_);
    const ssize_t n = ::sendfile(socket_->fd(), sendFileFd_, &offset, chunk);
    if (n > 0) {
      sendFileOffset_ += static_cast<std::int64_t>(n);
      sendFileRemaining_ -= static_cast<std::size_t>(n);
      continue;
    }
    if (n == 0) {
      finishSendFile(false);
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      channel_->enableWriting();
      return false;
    }
    finishSendFile(false);
    return false;
  }
  finishSendFile(true);
  return true;
#else
  finishSendFile(false);
  return false;
#endif
}

void TcpConnection::finishSendFile(bool ok) {
  ConnectionPtr guard = shared_from_this();
  (void)guard;
  const int fd = sendFileFd_;
  sendFileFd_ = -1;
  sendFileOffset_ = 0;
  sendFileRemaining_ = 0;
  sendFilePending_ = false;
  sendFileActive_ = false;

  std::function<void(bool)> done = sendFileDone_;
  sendFileDone_ = std::function<void(bool)>();
  closeFd(fd);
  if (done) {
    done(ok);
  }
  if (!ok && state_.load() == kConnected) {
    forceCloseInLoop();
  }
}

void TcpConnection::cancelSendFile() {
  ConnectionPtr guard = shared_from_this();
  (void)guard;
  const int fd = sendFileFd_;
  sendFileFd_ = -1;
  sendFileOffset_ = 0;
  sendFileRemaining_ = 0;
  sendFilePending_ = false;
  sendFileActive_ = false;

  std::function<void(bool)> done = sendFileDone_;
  sendFileDone_ = std::function<void(bool)>();
  closeFd(fd);
  if (done) {
    done(false);
  }
}

void TcpConnection::pauseRead() {
  if (loop_->isInLoopThread()) {
    readPaused_.store(true);
    channel_->disableReading();
    return;
  }
  ConnectionPtr self = shared_from_this();
  loop_->runInLoop([self]() {
    self->readPaused_.store(true);
    self->channel_->disableReading();
  });
}

void TcpConnection::resumeRead() {
  if (loop_->isInLoopThread()) {
    readPaused_.store(false);
    if (state_.load() == kConnected && !backpressure_ &&
        !channel_->isReading()) {
      channel_->enableReading();
    }
    return;
  }
  ConnectionPtr self = shared_from_this();
  loop_->runInLoop([self]() {
    self->readPaused_.store(false);
    if (self->state_.load() == kConnected && !self->backpressure_ &&
        !self->channel_->isReading()) {
      self->channel_->enableReading();
    }
  });
}

void TcpConnection::rearm() {
  if (state_.load() != kConnected) {
    return;
  }
  if (backpressure_) {
    return;  // 输出积压期间不读，避免内存继续膨胀
  }
  if (readPaused_.load()) {
    return;  // 大帧校验未完成或业务显式暂停
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
  if (outputBuffer_.readableBytes() == 0 && !sendFileActive_ &&
      !sendFilePending_) {
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
