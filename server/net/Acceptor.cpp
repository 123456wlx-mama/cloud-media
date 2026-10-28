#include "Acceptor.h"

#include <cerrno>
#include <cstring>

#include "EventLoop.h"
#include "cm/memory.h"

#if defined(__linux__)
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace net {

Acceptor::Acceptor(EventLoop* loop, const std::string& name)
    : loop_(loop), name_(name), port_(0), listening_(false), idleFd_(-1) {}

Acceptor::~Acceptor() {
  stop();
#if defined(__linux__)
  if (idleFd_ >= 0) {
    ::close(idleFd_);
    idleFd_ = -1;
  }
#endif
}

void Acceptor::setNewConnectionCallback(const NewConnectionCallback& cb) {
  newConnectionCallback_ = cb;
}

bool Acceptor::listen(uint16_t port) {
#if defined(__linux__)
  if (listening_) {
    return true;
  }
  const int fd = Socket::createNonblocking();
  if (fd < 0) {
    return false;
  }
  acceptSocket_ = cm::makeUnique<Socket>(fd);

  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

  struct sockaddr_in addr;
  ::memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) < 0) {
    return false;
  }
  if (::listen(fd, SOMAXCONN) < 0) {
    return false;
  }

  socklen_t addrLen = sizeof addr;
  ::getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &addrLen);
  port_ = ntohs(addr.sin_port);

  acceptChannel_ = cm::makeUnique<Channel>(loop_, fd);
  acceptChannel_->setReadCallback(std::bind(&Acceptor::handleRead, this));
  acceptChannel_->enableReading();

  if (idleFd_ < 0) {
    idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  }

  listening_ = true;
  return true;
#else
  (void)port;
  return false;
#endif
}

void Acceptor::stop() {
  // 关键：绝不在这里 release Channel。EventLoop::loop() 本轮已把 Channel* 收进
  // activeChannels，一旦信号回调先执行 stop()，随后解引用释放过的指针就会 SIGSEGV。
  // 正确做法：只摘除事件，对象留到 Acceptor 析构时在 loop 线程销毁。
  if (acceptChannel_) {
    acceptChannel_->disableAll();
    acceptChannel_->remove();
  }
  acceptSocket_.reset();
  listening_ = false;
}

void Acceptor::handleRead() {
#if defined(__linux__)
  if (!acceptSocket_ || !acceptSocket_->valid()) {
    return;
  }
  for (;;) {
    std::string peer;
    const int fd = Socket::acceptConnection(acceptSocket_->fd(), &peer);
    if (fd >= 0) {
      if (newConnectionCallback_) {
        newConnectionCallback_(fd, peer);
      } else {
        ::close(fd);
      }
      continue;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EMFILE || errno == ENFILE) {
      // 没有预留 fd 可借时不能再 continue：accept 会一直 EMFILE，
      // 单线程 Reactor 会在这里空转，整个服务失去响应。
      if (idleFd_ < 0) {
        break;
      }
      // fd 耗尽：借用预留 fd 的空位把这条连接 accept 出来再立即关掉，
      // 这样既清退了 backlog，又不会让监听 fd 一直可读导致 CPU 空转。
      ::close(idleFd_);
      const int accepted = ::accept(acceptSocket_->fd(), nullptr, nullptr);
      if (accepted >= 0) {
        ::close(accepted);
      }
      idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
      continue;
    }
    // EAGAIN：ET 的正常终止
    break;
  }
  // ET + EPOLLONESHOT：处理完必须重新武装，否则再也收不到新连接。
  // 若本轮回调里触发了 stop()（优雅停机），此时 fd 已关闭，不能再挂事件。
  if (listening_ && acceptChannel_) {
    acceptChannel_->enableReading();
  }
#endif
}

}  // namespace net
