// Phase 3 的自检服务：原样回显，SIGINT/SIGTERM 触发优雅退出。
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "EventLoop.h"
#include "SignalHandler.h"
#include "TcpServer.h"
#include "TimerQueue.h"

namespace {

void onMessage(const net::ConnectionPtr& conn, cm::Buffer* buf, net::Timestamp) {
  conn->send(buf->retrieveAllAsString());
}

}  // namespace

int main(int argc, char** argv) {
  uint16_t port = 8004;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
      port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else {
      std::fprintf(stderr, "usage: %s -p <port>\n", argv[0]);
      return 1;
    }
  }

  // 客户端断开后写数据不能把进程写死
  ::signal(SIGPIPE, SIG_IGN);

  net::EventLoop loop;
  net::TimerQueue timers(&loop);
  net::TcpServer server(&loop, "echo");
  net::SignalHandler signals(&loop);

  server.setMessageCallback(onMessage);

  // 优雅退出：停 accept -> 逐连接发 FIN -> 留 500ms 排空 -> 退出事件循环
  signals.setCallback([&loop, &server, &timers](int) {
    server.stopAccepting();
    server.shutdownAll();
    timers.runAfter(500, [&loop]() { loop.quit(); });
  });

  if (!signals.install({SIGINT, SIGTERM})) {
    std::fprintf(stderr, "install signal handler failed\n");
    return 1;
  }

  if (!server.start(port)) {
    std::fprintf(stderr, "listen on port %u failed\n", static_cast<unsigned>(port));
    return 1;
  }

  std::printf("echo server listening on 0.0.0.0:%u\n", static_cast<unsigned>(server.port()));
  std::fflush(stdout);

  loop.loop();

  // 事件循环已停，剩余连接由 server 析构统一关闭（fd 随 Socket 析构释放）
  return 0;
}
