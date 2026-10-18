// 多线程 echo 压测客户端：每个线程一个 epoll，各自负责一部分连接。
// 用法：bench_conn -c 2000 -t 60 [-s 512] [-T 4] [-h 127.0.0.1] [-p 8004]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__linux__)

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace {

// 延迟采样上限：避免长时间压测把内存吃满（p99 基于该采样估计）
const size_t kMaxLatencySamples = 100000;

struct Options {
  int connections = 1000;
  int seconds = 10;
  int payloadSize = 512;
  int threads = 4;
  std::string host = "127.0.0.1";
  uint16_t port = 8004;
};

struct ConnState {
  int fd;
  bool connecting;
  size_t expected;  // 已发出的字节数，等待回显同样多
  size_t received;
  int64_t sentAtUs;

  ConnState() : fd(-1), connecting(false), expected(0), received(0), sentAtUs(0) {}
};

struct WorkerStats {
  long long connected;
  long long connectFailed;
  long long completed;
  long long requestFailed;
  long long bytes;
  std::vector<int64_t> latencyUs;

  WorkerStats() : connected(0), connectFailed(0), completed(0), requestFailed(0), bytes(0) {}
};

int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool setNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void closeConn(int epfd, ConnState* conn) {
  if (conn->fd >= 0) {
    ::epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, nullptr);
    ::close(conn->fd);
    conn->fd = -1;
  }
}

// 单连接同时只有一个在途请求，所以这里几乎不会出现 EAGAIN
void sendRequest(ConnState* conn, const std::string& payload, WorkerStats* stats) {
  const ssize_t n = ::send(conn->fd, payload.data(), payload.size(), MSG_NOSIGNAL);
  if (n == static_cast<ssize_t>(payload.size())) {
    conn->expected = payload.size();
    conn->received = 0;
    conn->sentAtUs = nowUs();
    return;
  }
  conn->expected = 0;
  conn->received = 0;
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    return;  // 发送缓冲满，本轮跳过
  }
  ++stats->requestFailed;
}

void runWorker(const Options& opt, int connCount, WorkerStats* stats) {
  const int epfd = ::epoll_create1(EPOLL_CLOEXEC);
  if (epfd < 0) {
    return;
  }

  struct sockaddr_in addr;
  ::memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(opt.port);
  if (::inet_pton(AF_INET, opt.host.c_str(), &addr.sin_addr) != 1) {
    ::close(epfd);
    return;
  }

  std::vector<ConnState> conns(static_cast<size_t>(connCount));
  const std::string payload(static_cast<size_t>(opt.payloadSize), 'x');

  for (int i = 0; i < connCount; ++i) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      ++stats->connectFailed;
      continue;
    }
    setNonBlocking(fd);
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    ConnState& conn = conns[static_cast<size_t>(i)];
    conn.fd = fd;

    const int rc = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr);
    bool connecting = false;

    if (rc == 0) {
      ++stats->connected;
    } else if (errno == EINPROGRESS) {
      connecting = true;
    } else {
      ::close(fd);
      conn.fd = -1;
      ++stats->connectFailed;
      continue;
    }

    conn.connecting = connecting;

    struct epoll_event ev;
    ::memset(&ev, 0, sizeof ev);
    ev.events = connecting ? (EPOLLIN | EPOLLOUT) : EPOLLIN;
    ev.data.u32 = static_cast<uint32_t>(i);
    ::epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

    if (!connecting) {
      sendRequest(&conn, payload, stats);
    }
  }

  const int64_t deadline = nowUs() + static_cast<int64_t>(opt.seconds) * 1000000;
  struct epoll_event events[256];

  while (nowUs() < deadline) {
    const int n = ::epoll_wait(epfd, events, 256, 100);
    for (int i = 0; i < n; ++i) {
      const uint32_t index = events[i].data.u32;
      if (index >= conns.size()) {
        continue;
      }
      ConnState& conn = conns[index];
      if (conn.fd < 0) {
        continue;
      }

      if (events[i].events & (EPOLLERR | EPOLLHUP)) {
        const bool wasConnecting = conn.connecting;
        closeConn(epfd, &conn);
        if (wasConnecting) {
          ++stats->connectFailed;
        } else {
          ++stats->requestFailed;
        }
        continue;
      }

      if (conn.connecting && (events[i].events & EPOLLOUT)) {
        int err = 0;
        socklen_t len = sizeof err;
        ::getsockopt(conn.fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
          closeConn(epfd, &conn);
          ++stats->connectFailed;
          continue;
        }
        conn.connecting = false;
        ++stats->connected;

        // 连上后只关心可读，避免 EPOLLOUT 常驻导致空转
        struct epoll_event ev;
        ::memset(&ev, 0, sizeof ev);
        ev.events = EPOLLIN;
        ev.data.u32 = index;
        ::epoll_ctl(epfd, EPOLL_CTL_MOD, conn.fd, &ev);

        sendRequest(&conn, payload, stats);
        continue;
      }

      if (events[i].events & EPOLLIN) {
        char buf[8192];
        for (;;) {
          const ssize_t r = ::recv(conn.fd, buf, sizeof buf, 0);
          if (r > 0) {
            conn.received += static_cast<size_t>(r);
            stats->bytes += r;
            if (conn.expected > 0 && conn.received >= conn.expected) {
              if (stats->latencyUs.size() < kMaxLatencySamples) {
                stats->latencyUs.push_back(nowUs() - conn.sentAtUs);
              }
              ++stats->completed;
              conn.expected = 0;
              conn.received = 0;
              sendRequest(&conn, payload, stats);
            }
            continue;
          }
          if (r == 0) {
            closeConn(epfd, &conn);
            ++stats->requestFailed;
            break;
          }
          if (errno == EINTR) {
            continue;
          }
          break;  // EAGAIN
        }
      }
    }
  }

  for (size_t i = 0; i < conns.size(); ++i) {
    if (conns[i].fd >= 0) {
      ::close(conns[i].fd);
      conns[i].fd = -1;
    }
  }
  ::close(epfd);
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
      opt.connections = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
      opt.seconds = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
      opt.payloadSize = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
      opt.threads = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
      opt.host = argv[++i];
    } else if (std::strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
      opt.port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else {
      std::fprintf(stderr,
                   "usage: %s -c <connections> -t <seconds> [-s payload] [-T threads] "
                   "[-h host] [-p port]\n",
                   argv[0]);
      return 1;
    }
  }

  if (opt.connections <= 0 || opt.seconds <= 0 || opt.payloadSize <= 0) {
    std::fprintf(stderr, "connections/seconds/payload must be positive\n");
    return 1;
  }
  if (opt.threads <= 0) {
    opt.threads = 1;
  }
  if (opt.threads > opt.connections) {
    opt.threads = opt.connections;
  }

  std::vector<WorkerStats> stats(static_cast<size_t>(opt.threads));
  std::vector<std::thread> workers;

  const int base = opt.connections / opt.threads;
  const int extra = opt.connections % opt.threads;
  for (int i = 0; i < opt.threads; ++i) {
    const int count = base + (i < extra ? 1 : 0);
    workers.push_back(std::thread(runWorker, std::cref(opt), count, &stats[static_cast<size_t>(i)]));
  }

  const int64_t beginUs = nowUs();
  for (size_t i = 0; i < workers.size(); ++i) {
    workers[i].join();
  }
  const double elapsedSec = static_cast<double>(nowUs() - beginUs) / 1000000.0;

  long long connected = 0;
  long long connectFailed = 0;
  long long completed = 0;
  long long requestFailed = 0;
  long long bytes = 0;
  std::vector<int64_t> latencies;

  for (size_t i = 0; i < stats.size(); ++i) {
    connected += stats[i].connected;
    connectFailed += stats[i].connectFailed;
    completed += stats[i].completed;
    requestFailed += stats[i].requestFailed;
    bytes += stats[i].bytes;
    latencies.insert(latencies.end(), stats[i].latencyUs.begin(), stats[i].latencyUs.end());
  }

  double p50 = 0.0;
  double p99 = 0.0;
  if (!latencies.empty()) {
    std::sort(latencies.begin(), latencies.end());
    p50 = static_cast<double>(latencies[latencies.size() * 50 / 100]);
    p99 = static_cast<double>(latencies[latencies.size() * 99 / 100]);
  }

  const double qps = elapsedSec > 0.0 ? static_cast<double>(completed) / elapsedSec : 0.0;
  const double connSuccess =
      opt.connections > 0 ? 100.0 * static_cast<double>(connected) / opt.connections : 0.0;
  const long long requests = completed + requestFailed;
  const double reqSuccess =
      requests > 0 ? 100.0 * static_cast<double>(completed) / static_cast<double>(requests) : 0.0;

  std::printf(
      "conns=%d connected=%lld connect_failed=%lld conn_success=%.2f%% "
      "requests=%lld qps=%.1f req_success=%.2f%% p50=%.0fus p99=%.0fus bytes=%lld elapsed=%.1fs\n",
      opt.connections, connected, connectFailed, connSuccess, requests, qps, reqSuccess, p50, p99,
      bytes, elapsedSec);
  return 0;
}

#else

int main() {
  std::fprintf(stderr, "bench_conn requires Linux (epoll).\n");
  return 1;
}

#endif
