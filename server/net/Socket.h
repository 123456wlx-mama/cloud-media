#pragma once

#include <string>

namespace net {

// fd 的 RAII 所有者：析构自动 close，禁止拷贝，允许 move。
class Socket {
 public:
  explicit Socket(int fd);
  ~Socket();

  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  int fd() const { return fd_; }
  bool valid() const { return fd_ >= 0; }

  void shutdownWrite();
  void close();

  // 非阻塞 + CLOEXEC 的监听/连接套接字；失败返回 -1
  static int createNonblocking();
  // accept4 一次拿到非阻塞 fd，peer 输出 "ip:port"
  static int acceptConnection(int listenFd, std::string* peer);

 private:
  int fd_;
};

}  // namespace net
