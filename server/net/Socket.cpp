#include "Socket.h"

#include <cerrno>

#if defined(__linux__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace net {

Socket::Socket(int fd) : fd_(fd) {}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

void Socket::shutdownWrite() {
#if defined(__linux__)
  if (fd_ >= 0) {
    ::shutdown(fd_, SHUT_WR);
  }
#endif
}

void Socket::close() {
#if defined(__linux__)
  if (fd_ >= 0) {
    ::close(fd_);
  }
#endif
  fd_ = -1;
}

int Socket::createNonblocking() {
#if defined(__linux__)
  return ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#else
  return -1;
#endif
}

int Socket::acceptConnection(int listenFd, std::string* peer) {
#if defined(__linux__)
  struct sockaddr_in addr;
  socklen_t addrLen = sizeof addr;
  const int fd = ::accept4(listenFd, reinterpret_cast<struct sockaddr*>(&addr), &addrLen,
                           SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (fd >= 0 && peer != nullptr) {
    char ip[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip);
    *peer = std::string(ip) + ":" + std::to_string(static_cast<int>(ntohs(addr.sin_port)));
  }
  return fd;
#else
  (void)listenFd;
  (void)peer;
  return -1;
#endif
}

}  // namespace net
