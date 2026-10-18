#include <gtest/gtest.h>

#if defined(__linux__)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "EventLoop.h"
#include "TcpServer.h"

namespace {

bool waitFor(const std::function<bool()>& pred, int timeoutMs) {
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return pred();
}

// 在独立线程里跑 loop + server，主线程用阻塞 socket 当客户端
class EchoServerHarness {
 public:
  EchoServerHarness() : loop_(nullptr), port_(0), failed_(false) {
    thread_ = std::thread([this]() { run(); });
    std::unique_lock<std::mutex> lock(mutex_);
    // start 失败时也要唤醒，否则构造函数会永久等待
    cond_.wait(lock, [this]() { return (loop_ != nullptr && port_ != 0) || failed_; });
  }

  ~EchoServerHarness() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (loop_ != nullptr) {
        loop_->quit();
      }
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  uint16_t port() const { return port_; }
  int closeCount() const { return closeCount_.load(); }

 private:
  void run() {
    net::EventLoop loop;
    net::TcpServer server(&loop, "test");

    server.setMessageCallback([](const net::ConnectionPtr& conn, cm::Buffer* buf,
                                 net::Timestamp) {
      conn->send(buf->retrieveAllAsString());
    });
    server.setConnectionCallback([this](const net::ConnectionPtr& conn) {
      if (!conn->connected()) {
        closeCount_.fetch_add(1);
      }
    });

    if (!server.start(0)) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_ = true;
      }
      cond_.notify_all();
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      loop_ = &loop;
      port_ = server.port();
    }
    cond_.notify_all();

    loop.loop();
  }

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cond_;
  net::EventLoop* loop_;
  uint16_t port_;
  bool failed_;
  std::atomic<int> closeCount_{0};
};

int connectTo(uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  struct sockaddr_in addr;
  ::memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

}  // namespace

// 1) 单连接 echo：发什么收回什么
TEST(TcpServerTest, EchoSingleConnection) {
  EchoServerHarness harness;
  ASSERT_NE(harness.port(), 0);

  const int fd = connectTo(harness.port());
  ASSERT_GE(fd, 0);

  const char* msg = "hello";
  ASSERT_EQ(::write(fd, msg, 5), 5);

  char buf[32] = {0};
  const ssize_t n = ::read(fd, buf, sizeof buf - 1);
  EXPECT_EQ(n, 5);
  EXPECT_STREQ(buf, "hello");

  ::close(fd);
}

// 2) 对端 FIN 后：连接必须被回收，close 回调触发一次
TEST(TcpServerTest, CloseCallbackOnPeerFin) {
  EchoServerHarness harness;
  ASSERT_NE(harness.port(), 0);

  const int fd = connectTo(harness.port());
  ASSERT_GE(fd, 0);
  ::close(fd);

  EXPECT_TRUE(waitFor([&harness]() { return harness.closeCount() == 1; }, 2000));
}

#else  // !__linux__

TEST(TcpServerTest, EchoSingleConnection) {
  GTEST_SKIP() << "epoll path is Linux only";
}

TEST(TcpServerTest, CloseCallbackOnPeerFin) {
  GTEST_SKIP() << "epoll path is Linux only";
}

#endif
