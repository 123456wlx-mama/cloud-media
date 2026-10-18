# Phase 3 实施计划：服务端网络层（Reactor / epoll ET / 定时器 / 优雅退出）

**Created:** 2026-10-18
**Status:** In Progress
**Source Spec:** [`docs/specs/04-网络设计.md`](../specs/04-网络设计.md)（配套：`终端改造路线图.md` Phase 3）
**执行环境:** Ubuntu 16.04 / kernel 4.4 / g++ 5.4 / cmake 3.5 / 纯 C++11

**执行记录（2026-10-18）**
- 已写：Task 0 的构建骨架（根 `CMakeLists.txt`、`server/`、`server/net/`、`tests/`）、Task 1 的 `Channel/Poller/EpollPoller`、Task 2 的 `EventLoop/EventLoopThread`、`tests/test_event_loop.cpp`（3 例）、`common/memory.h`。
- Windows 通过 `#ifdef __linux__` 保留 epoll 路径，非 Linux 编译为空实现（短睡眠轮询）。
- 第一轮追加（同日）：`common/base/buffer`（readv + 64KB 栈缓冲，读到 EAGAIN）、`common/base/thread_pool`（std::mutex + condition_variable，停机排空队列）、`Socket/Acceptor/TcpConnection/TcpServer`、`test_thread_pool`（3 例）、`test_tcp_server`（2 例，非 Linux 用 GTEST_SKIP 占位）。
- 第二轮追加（同日）：`TimerQueue`（std::set 最小堆 + timerfd；非 Linux 用条件变量线程模拟）、`SignalHandler`（self-pipe，二次信号 `_exit`）、`TcpServer::stopAccepting/shutdownAll`（优雅退出分两步）、`server/examples/echo_server`、`benchmarks/bench_conn`（每线程一个 epoll，输出连接成功率/QPS/P50/P99）。
- **未验证**：本机未执行任何构建/测试命令，Task 1~8 的 DoD 均未勾选，需在 Ubuntu 16.04 上跑 `ctest` + echo/bench 压测后才能标 Done。

## Overview

**Goal:** 交付一个可压测、可测试、可优雅退出的 TCP 网络层（主从 Reactor + epoll ET/ONESHOT + 定时器 + 信号处理），作为 Phase 4/5 的承载底座。

**Approach:** 自底向上、每层自带可运行验证：先打通构建与测试骨架，再按「fd 抽象 → 事件循环 → 缓冲/接受/连接 → 写路径与背压 → 服务器与生命周期 → 定时器/心跳 → 退出 → 压测工具」推进。ET + ONESHOT 的行为风险集中在 §3.3~3.5，对应单元/集成测试先行。**不写业务逻辑**，echo 即验收。

**Tech Stack:** C++11（`std::thread/mutex/atomic/chrono/function`）、Linux epoll（`epoll_create1/ctl/wait`）、`eventfd`、`timerfd`、`accept4`、`sendfile`（Phase 5 才用）、GoogleTest 1.10.x。

---

## 前置条件与假设（全部已给安全默认，不阻塞开工）

| 项 | 现状 | 本计划的处理 |
|---|---|---|
| 仓库骨架（Phase 1） | 本目录**尚无** `CMakeLists.txt`、`server/`、`thirdparty/` | **Task 0 负责创建 Phase 3 所需的最小骨架**；若 Phase 1 骨架后续落地，以本任务的文件路径为准合并 |
| `docs/specs/01-架构设计.md` | 不存在 | 目录与命名以 `04-网络设计.md` §2.4 为准；若后续出现 01，冲突以 01 为准并回改本计划 |
| `legacy/server/linux-epoll` | 不存在 | 本阶段**不做迁移**，按设计文档全新实现；legacy 到位后仅用于 Phase 5 业务对照 |
| 命名空间 | 未定义 | 统一 `namespace net`；文件内不再套子命名空间 |
| gtest 来源 | 未引入 | `thirdparty/googletest` 放 **release-1.10.0**（1.11+ 要求 C++14，g++5.4 编不过）；若采用系统包，`apt install libgtest-dev` 后同样锁 1.10 |
| 构建命令 | 未建立 | **cmake 3.5 不支持 `cmake -S/-B`（3.13+）与 `ctest --test-dir`（3.20+）**，全文只用 `mkdir build && cd build && cmake .. && make` |

**构建硬约束（每个 Task 的验证都隐含）**
- `cmake_minimum_required(VERSION 3.5)`
- `set(CMAKE_CXX_STANDARD 11)` + `CMAKE_CXX_STANDARD_REQUIRED ON` + `CMAKE_CXX_EXTENSIONS OFF`
- 禁用 3.6+ 特性：不用 `gtest_discover_tests`(3.10)、`FetchContent`(3.11)、`target_link_options`(3.13)
- 测试注册用 `add_test(NAME x COMMAND x)`，不用自动发现

---

## 文件与职责

| 文件/模块 | 动作 | 职责 | 关键约束 |
|---|---|---|---|
| `CMakeLists.txt`（根） | Create | 全局标准、警告、ASan 选项、子目录 | cmake 3.5；`-Wall -Wextra` |
| `server/net/CMakeLists.txt` | Create | 产出静态库 `net` | 不链接 MySQL/Redis |
| `server/net/Socket.{h,cpp}` | Create | fd 的 RAII 所有者 + SocketOps | 析构 `close`；禁止拷贝，允许 move |
| `server/net/InetAddress.{h,cpp}` | Create | 地址封装 + `toHostPort()` | 仅 IPv4（本阶段） |
| `server/net/Channel.{h,cpp}` | Create | fd + 关注事件 + 回调 + `tie(weak_ptr)` | 不拥有 fd |
| `server/net/Poller.{h,cpp}` | Create | 轮询器抽象接口 | 纯虚，供将来 poll 实现 |
| `server/net/EpollPoller.{h,cpp}` | Create | epoll 具体实现 + activeChannels | `epoll_create1(EPOLL_CLOEXEC)` |
| `server/net/EventLoop.{h,cpp}` | Create | 循环、`runInLoop/queueInLoop`、eventfd 唤醒 | `isInLoopThread` 断言 |
| `server/net/EventLoopThread.{h,cpp}` | Create | loop + std::thread 绑定，启动同步 | 构造后需等待 loop 就绪 |
| `server/net/EventLoopThreadPool.{h,cpp}` | Create | sub Reactor 池，round-robin | `n<=1` 退化为 main loop |
| `server/net/Buffer.{h,cpp}` | Create | 可读写索引 + `readFd`（readv + 64KB 栈缓冲） | `std::vector<char>`，禁裸 new |
| `server/net/Acceptor.{h,cpp}` | Create | listen fd + `accept4` 循环 | EMFILE 退避 |
| `server/net/TcpConnection.{h,cpp}` | Create | 连接状态机、读写、背压、shutdown/forceClose | 全部状态变更限 loop 线程 |
| `server/net/TcpServer.{h,cpp}` | Create | 连接表、回调装配、removeConnection | 表是唯一 owner |
| `server/net/TimerQueue.{h,cpp}`、`TimerId.h` | Create | 最小堆 + timerfd | `steady_clock`；支持取消 |
| `server/net/SignalHandler.{h,cpp}` | Create | self-pipe + SIGINT/SIGTERM | handler 只 `write` |
| `server/net/Callbacks.h` | Create | `std::function` 别名、`Timestamp` | 无实现 |
| `server/net/tests/*.cpp` | Create/Test | 见「gtest 清单」 | 每个用例独立可跑 |
| `benchmarks/bench_conn.cpp` | Create | 多连接 echo 压测客户端 | 输出连接数/失败数/QPS |
| `server/net/examples/echo_server.cpp` | Create | 压测用的 echo 服务 | 仅装配，不含业务 |

**边界（写死）**：`net` 库**不**依赖 `common/` 的协议 codec；解码通过 `MessageCallback` 由上层注入。`net` **不**调用日志之外的任何外部服务。不做 UDP / TLS / 多进程。

---

## 任务拆分

### Task 0：构建骨架与测试基座

**Goal:** 让 `make && ctest` 在 Ubuntu 16.04 + cmake 3.5 上跑通空测试，后续所有任务都在这套骨架上增量。

**Files:**
- Create: `CMakeLists.txt`、`server/CMakeLists.txt`、`server/net/CMakeLists.txt`、`server/net/tests/CMakeLists.txt`
- Create: `thirdparty/googletest/`（源码或 `GTEST_ROOT` 指向系统安装）
- Create: `server/net/tests/test_smoke.cpp`

**Steps:**
- [ ] 根 `CMakeLists.txt`：`cmake_minimum_required(VERSION 3.5)`、`project(cloud_media CXX)`、`set(CMAKE_CXX_STANDARD 11)`、`CMAKE_CXX_STANDARD_REQUIRED ON`、`CMAKE_CXX_EXTENSIONS OFF`、`set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)`（所有可执行文件统一在 `build/bin/`）、`add_compile_options(-Wall -Wextra -Wno-unused-parameter)`、`option(ENABLE_ASAN "..." OFF)`（开启时加 `-fsanitize=address,undefined -fno-omit-frame-pointer` 到编译与链接）、`enable_testing()`、`add_subdirectory(server)`。
- [ ] `server/net/CMakeLists.txt`：`add_library(net STATIC ...)`，**只列已存在的源文件**（每加一个模块就追加，不用 GLOB），`target_link_libraries(net PUBLIC pthread)`。
- [ ] `server/net/tests/CMakeLists.txt`：链接 `net` 与 `gtest_main`；**每个测试文件一个 `add_executable` + `add_test`**（cmake 3.5 无 `gtest_discover_tests`）。
- [ ] `test_smoke.cpp` 只放 `TEST(Smoke, Builds) { EXPECT_EQ(1, 1); }`。
- [ ] Run: `mkdir -p build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j4 && ctest --output-on-failure`
  - Expected: 编译零警告；`100% tests passed, 1 test from 1 test suite`。
- [ ] Run: `cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON && make -j4 && ctest --output-on-failure`
  - Expected: 同样通过，无 ASan 报告（证明 ASan 链路可用）。

**Security/Boundary Notes:** gtest 源码只放 `thirdparty/`，不打进 `net` 库。

---

### Task 1：fd 抽象（Socket / InetAddress）+ Channel + EpollPoller

**Goal:** 把「一个 fd + 一组回调 + 关注事件」跑通，能对一个 socketpair 做到「可读事件到达 → 回调执行」。

**Files:**
- Create: `server/net/Socket.{h,cpp}`、`InetAddress.{h,cpp}`、`Channel.{h,cpp}`、`Poller.{h,cpp}`、`EpollPoller.{h,cpp}`、`Callbacks.h`
- Test: `server/net/tests/test_socket.cpp`、`test_channel_poller.cpp`

**Steps:**
- [ ] 写 `test_socket.cpp`：非阻塞/CLOEXEC 位、`getSocketError`、move 后源 fd 为 -1、析构后 fd 被关闭（用 `fcntl(fd, F_GETFD)` 返回 -1 断言）。
- [ ] `Socket` 提供服务端/客户端创建、`bind/listen/accept4`、`shutdownWrite`、`setTcpNoDelay/setReuseAddr/setKeepAlive`；套接字一律 `SOCK_NONBLOCK | SOCK_CLOEXEC`。
- [ ] 写 `test_channel_poller.cpp`：socketpair 写 1 字节 → `poller.poll(100ms)` 返回该 channel → `handleEvent` 里 `revents` 含 `EPOLLIN`；`disableReading` 后再 poll 100ms 无事件；`remove()` 后 poll 无该 fd。
- [ ] `EpollPoller`：`epoll_create1(EPOLL_CLOEXEC)`；`updateChannel` 用 `EPOLL_CTL_ADD/MOD`；`removeChannel` 用 `DEL` 并从 `channels_`（`std::map<int, Channel*>`）摘除；`fillActiveChannels` 按 maxEvents 处理事件掩码 `EPOLLERR|EPOLLHUP|EPOLLRDHUP`。
- [ ] `Channel` 提供 `enableReading/Writing`、`disableReading/Writing/All`、`setReadCallback/...`、`tie(const std::weak_ptr<void>&)`；`handleEvent` 首行 `tie_.lock()` 保活。
- [ ] Run: `cd build && make -j4 && ctest -R "net_socket|net_channel" --output-on-failure`
  - Expected: 全绿；`test_channel_poller` 的 3 个阶段断言全部成立。

**Security/Boundary Notes:** `EpollPoller` 不拥有 Channel 生命周期，只存裸 `Channel*`；Channel 必须在 poller 移除后才销毁（由 TcpConnection 的 `destroyInLoop` 保证）。

---

### Task 2：EventLoop + eventfd 唤醒 + EventLoopThread(Pool)

**Goal:** 跨线程投递任务可靠执行，`quit()` 能立即生效（不被 `epoll_wait` 阻塞）。

**Files:**
- Create: `EventLoop.{h,cpp}`、`EventLoopThread.{h,cpp}`、`EventLoopThreadPool.{h,cpp}`
- Test: `server/net/tests/test_event_loop.cpp`

**Steps:**
- [ ] `EventLoop` 构造时建 `eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC)`，包成 Channel 常驻监听 `EPOLLIN`；`doPendingFunctors` 用 `swap` 交换到局部再执行（避免回调里再投递导致死循环/迭代器失效）。
- [ ] `runInLoop`：同线程直接执行，否则 `queueInLoop`；`queueInLoop` 用 `std::mutex` 保护 `pendingFunctors_`，只在「非本线程 或 正在执行回调」时 `wakeup()`（写 8 字节 uint64）。
- [ ] `loop()`：`while(!quit_) { poller_->poll(timeoutMs); 处理事件; doPendingFunctors(); }`；`epoll_wait` 返回 `EINTR` 时 `continue`。
- [ ] `EventLoopThread`：构造即起线程，用 `std::condition_variable` 等 loop 建好再返回；析构 `quit() + join()`（**不得 join 自身**，加 `assert`）。
- [ ] 写测试：`RunInLoopSameThreadImmediate`（同线程同步执行）、`QueueInLoopFromOtherThread`（1000 次投递全部执行，`std::atomic<int>` 计数）、`QuitWakesUpBlockedPoll`（`quit()` 后 `loop()` 在 100ms 内返回）、`RunInLoopFromCallbackDoesNotHang`（回调里再 `runInLoop`）。
- [ ] Run: `cd build && make -j4 && ctest -R net_event_loop --output-on-failure`
  - Expected: 4 个用例全绿；`QuitWakesUpBlockedPoll` 用时 < 200ms。

**Security/Boundary Notes:** `quit_` 用 `std::atomic<bool>`；禁止 `volatile`。

---

### Task 3：Buffer + 读路径（ET 读到 EAGAIN）

**Goal:** 做到「一次事件把内核缓冲读空」，并把完整数据交给回调。

**Files:**
- Create: `Buffer.{h,cpp}`
- Modify: `TcpConnection.{h,cpp}`（本任务先只实现 `handleRead`）
- Test: `tests/test_buffer.cpp`

**Steps:**
- [ ] 写 `test_buffer.cpp`：`AppendRetrieve`、`RetrievePartialLeavesRest`、`GrowBeyondInitialCapacity`（追加 1MB）、`RetrieveAllReturnsEmpty`、`ReadFdLoopsUntilEagain`（socketpair 对端写 200KB 后 `readFd`，断言读满 200KB 且返回 `EAGAIN`）。
- [ ] `Buffer`：`std::vector<char> buf_` + `readIndex_/writeIndex_`；`readFd(fd, int* savedErrno)` 用 `readv` 两段 iovec（现有可写区 + 栈上 64KB `extrabuf`），循环到 `EAGAIN`；prepend 用 `memmove` 只搬未读部分。
- [ ] `TcpConnection::handleRead`：按设计 §3.3 实现；`n==0` 时**先把 inputBuffer 里剩余完整包交给回调**再关闭。
- [ ] 集成测试 `test_read_eagain.cpp`：**不依赖 TcpServer**，用 `socketpair` 造出 `TcpConnection`（`socketpair[0]` 交给连接，`socketpair[1]` 当对端），在 `EventLoopThread` 里跑；对端一次 `write` 1MB，回调只累加字节数不回复；断言单次事件收满 1MB（证明读到了 EAGAIN，未提前返回），并断言半包不被误回调。
- [ ] Run: `cd build && make -j4 && ctest -R "net_buffer|net_read" --output-on-failure`
  - Expected: 全绿；`ReadFdLoopsUntilEagain` 与 `SingleEventReadsUntilEagain` 均通过。

**Security/Boundary Notes:** `readFd` 不信任 `ssize_t`，负值必须查 errno；对端 `n==0` 与 `n<0&&EAGAIN` 必须区分（前者关闭，后者继续等）。

---

### Task 4：写路径、EPOLLOUT 按需武装、背压

**Goal:** 任意线程 `send()` 都能正确送达；对端不收数据时内存不失控、CPU 不空转。

**Files:**
- Modify: `TcpConnection.{h,cpp}`（`sendInLoop/handleWrite/enableWriting/disableWriting`）
- Test: `tests/test_write_path.cpp`

**Steps:**
- [ ] `send(data,len)`：`runInLoop(std::bind(&TcpConnection::sendInLoop, this, ...))`；**注意** 跨线程传 `std::string` 拷贝而非裸指针（调用方栈缓冲不能跨线程）。
- [ ] `sendInLoop`：缓冲空时直接 `::send(..., MSG_NOSIGNAL)`；部分写/ EAGAIN 入 `outputBuffer_` 并 `enableWriting()`；全程不注册常驻 `EPOLLOUT`。
- [ ] `handleWrite`：循环写到 EAGAIN → `disableWriting()`；写空后若处于背压停读状态则恢复 `enableReading()`；`kDisconnecting` 且写空 → `shutdownWrite()`。
- [ ] 背压：`outputBuffer_.readableBytes() > highWaterMark` 时 `disableReading()` 并置 `backpressure_=true`；降到 `lowWaterMark` 以下恢复。水位可配（默认 8MB/2MB，测试用小值）。
- [ ] 写测试：`SendPartialThenEpollout`（`SO_SNDBUF` 调小 + 发 1MB，断言先部分写、后续由 `EPOLLOUT` 补完且总量一致）、`SendFromOtherThreadDelivered`、`NoEpolloutWhenIdle`（空闲 500ms 内 poll 不返回写事件）、`BackpressureStopsReading`。
- [ ] Run: `cd build && make -j4 && ctest -R net_write --output-on-failure`
  - Expected: 全绿；`NoEpolloutWhenIdle` 通过（这条直接对应「空闲 CPU 100%」事故）。

**Security/Boundary Notes:** 所有 `send` 必须带 `MSG_NOSIGNAL`；启动时另设 `SIGPIPE` 忽略（Task 7 落地）。

---

### Task 5：Acceptor + TcpServer + 连接生命周期

**Goal:** 完成「accept → 建连 → 收发 → 关闭 → 释放」全链路，且析构不发生在事件回调栈内。

**Files:**
- Create: `Acceptor.{h,cpp}`、`TcpServer.{h,cpp}`
- Modify: `TcpConnection.{h,cpp}`（`connectEstablished/connectDestroyed/destroyInLoop`）

> 说明：本任务只实现 `TcpServer::stop()` 的**最小版**（停 accept + 关闭全部连接），分阶段优雅退出（drain/deadline/join 顺序）在 Task 7 升级。
- Test: `tests/test_tcp_server.cpp`、`tests/test_lifecycle.cpp`

**Steps:**
- [ ] `Acceptor`：`accept4(listenFd, ..., SOCK_NONBLOCK|SOCK_CLOEXEC)` 循环直到 EAGAIN；`EMFILE/ENFILE` 时记 WARN 并 `std::this_thread::sleep_for(100ms)`（**不空转**）；每接受一个连接回调 `newConnection(fd, peerAddr)`。
- [ ] `TcpServer`：`connections_` 为 `std::map<std::string, ConnectionPtr>`，key=`peerAddr#seq`；`ioThreads<=1` 时用 main loop，否则 round-robin 从池里取 loop，并在该 loop 内 `runInLoop` 完成 `TcpConnection` 构造（**对象在所属 loop 线程创建**）。
- [ ] 连接构造顺序：`std::make_shared<TcpConnection>` → `channel_->tie(conn)` → `channel_->set*Callback` → `loop->runInLoop(connectEstablished)`；禁止在构造函数里 `shared_from_this()`。
- [ ] `removeConnection`：`connections_.erase(key)` 后 `loop->queueInLoop([conn]{ conn->destroyInLoop(); })`；`destroyInLoop` 内 `disableAll + remove + socket close`。
- [ ] 写测试：`AcceptThenEcho`、`PeerFinTriggersCloseCallback`、`SharedPtrOutlivesClose`（回调里留 `weak_ptr`，关闭后 `lock()==nullptr`）、`DestroyNotInsideCallback`（`forceClose` 后断言析构标志在回调返回之后才置位）、`ServerStopClosesAll`（10 连接，stop 后计数归零）。
- [ ] Run: `cd build && make -j4 && ctest -R "net_server|net_lifecycle" --output-on-failure`
  - Expected: 全绿，无 `bad_weak_ptr`、无 `use-after-free`。

**Security/Boundary Notes:** 连接表是唯一 owner；任何回调捕获必须是 `weak_ptr`，禁止自引用环。fd 关闭前必须 `EPOLL_CTL_DEL`。

---

### Task 6：TimerQueue（timerfd + 最小堆）与心跳扫描

**Goal:** 定时器可增删、可取消、取消后不触发；心跳踢除不引入每连接堆操作。

**Files:**
- Create: `TimerQueue.{h,cpp}`、`TimerId.h`
- Modify: `TcpConnection.{h,cpp}`（`lastActiveMs_`）、`TcpServer.{h,cpp}`（心跳 tick 扫描）
- Test: `tests/test_timer_queue.cpp`

**Steps:**
- [ ] 数据结构：`std::set<TimerEntry>`，`TimerEntry{Timestamp when; int64_t seq; TimerCallback cb;}`；`TimerId{const void* token; int64_t seq;}`。
- [ ] `timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC)` 注册进本 loop 的 epoll；**时刻换算**：构造时记录一次 `delta = steady_now - mono_now`，之后 `it_value` 用 `when - delta` 传 `TFD_TIMER_ABSTIME`（否则会出现「每次重设都偏移」的经典 bug）；只在最早到期时间变化时 `timerfd_settime`。
- [ ] `handleRead` 读 8 字节后，`while (begin->when <= now)` 逐个 `erase` 到局部再执行回调（**先摘再调**，允许回调内取消其它定时器）。
- [ ] `cancel(id)`：按 `seq` 校验后 `erase`；已执行/已取消的 id 再次 cancel 必须无副作用。
- [ ] 心跳：`TcpServer::start()` 注册一个 `runEvery(1000ms)`，遍历 `connections_` 比较 `lastActiveMs_`，超 `idleTimeoutMs` 调 `shutdown()`；单次耗时 > 10ms 打 WARN。
- [ ] 写测试：`RunAfterFiresOnce`、`RunEveryFiresN`、`CancelBeforeFire`、`CancelInsideCallback`、`SameDeadlineOrderBySeq`、`CancelThenRecreateDoesNotFire`、`PastDeadlineFiresImmediately`、`TenThousandTimersAllFire`（只断言全部触发，不做性能断言）。
- [ ] Run: `cd build && make -j4 && ctest -R net_timer --output-on-failure`
  - Expected: 全绿；8 个用例无 flaky（用 ≥30ms 的间隔，不用 0ms）。

**Security/Boundary Notes:** 定时器回调只在本 loop 线程执行；回调里做重活必须投业务池。禁止 `system_clock`。

---

### Task 7：SignalHandler + 优雅退出

**Goal:** SIGINT/SIGTERM 后 5s 内干净退出，在途写入不丢、无 fd/内存泄漏。

**Files:**
- Create: `SignalHandler.{h,cpp}`
- Modify: `TcpServer.{h,cpp}`（`stop()` 分阶段）、`examples/echo_server.cpp`（装配退出流程）
- Test: `tests/test_shutdown.cpp`

**Steps:**
- [ ] `SignalHandler`：`pipe2(O_NONBLOCK|O_CLOEXEC)`，读端包 Channel 挂到 main loop；`sigaction` 注册 SIGINT/SIGTERM，**不加 `SA_RESTART`**；handler 内 `int saved=errno; write(fd,"x",1); errno=saved;`；第二次信号 `_exit(1)`。
- [ ] 启动时 `signal(SIGPIPE, SIG_IGN)`。
- [ ] `TcpServer::stop()` 严格按设计 §6.1：停 accept → 各 loop `runInLoop` 逐连接 shutdown（deadline = now+2000ms）→ 到期 `forceClose` → 停业务池 → 停 sub loops 并 join → 停 main loop → 清空 `connections_` → 关资源池钩子 → 日志 flush。
- [ ] 写测试：`SigintQuitsWithinOneTick`（`raise(SIGINT)` 后 loop 100ms 内退出）、`PendingWriteFlushedBeforeClose`（大包发送中触发 stop，客户端收满字节）、`NoFdLeakAfterStop`（统计 `/proc/self/fd` 前后差值 ≤ 2）。
- [ ] Run: `cd build && make -j4 && ctest -R net_shutdown --output-on-failure`
  - Expected: 全绿。
- [ ] Run: `cd build && cmake .. -DENABLE_ASAN=ON && make -j4 && ctest -R net_shutdown --output-on-failure`
  - Expected: 全绿且无 LeakSanitizer 报告。

**Security/Boundary Notes:** handler 内禁止 `printf/malloc/日志`；退出顺序不得交换（先 join 再关池）。

---

### Task 8：压测工具与 Phase 3 验收

**Goal:** 产出可写进简历的 2000 连接数据，并暴露 CPU/fd 稳定性。

**Files:**
- Create: `benchmarks/bench_conn.cpp`、`benchmarks/CMakeLists.txt`
- Create: `server/net/examples/echo_server.cpp`
- Create: `docs/benchmark/Phase3-网络层.md`（结果记录）

**Steps:**
- [ ] 根 `CMakeLists.txt` 追加 `add_subdirectory(benchmarks)`；`server/net/CMakeLists.txt` 追加 `echo_server` 可执行目标。
- [ ] `bench_conn`：`-c 连接数 -t 秒数 -s 包大小 -h host -p port`；多线程各自 epoll；统计成功连接数、失败数、收发字节、QPS、P99 延迟（`steady_clock` 记录），结束时输出一行 CSV。
- [ ] `echo_server`：`-p 端口 -i io_threads`，`MessageCallback` 原样回写，`SIGINT` 走 Task 7 流程。
- [ ] Run: `cd build && make -j4`
- [ ] Run: `./bin/echo_server -p 8004 -i 1 &` 然后 `./bin/bench_conn -c 2000 -t 600 -s 512`
  - Expected: 失败数为 0；600s 内无断连；QPS 稳定（记录数值）。
- [ ] 压测中另开终端：
  - `ls /proc/$(pgrep echo_server)/fd | wc -l` → 首尾差值 ≤ 2
  - `top -H -p $(pgrep echo_server)` → 空闲主线程 CPU < 2%，无单线程长期 100%
  - `ss -s` 或 `netstat -anp | grep 8004 | wc -l` → 约 2000
- [ ] 压测中发 `SIGINT`：`kill -INT $(pgrep echo_server)`，用 `time` 包裹启动脚本测总退出时间 → < 5s
- [ ] 把上述数据写入 `docs/benchmark/Phase3-网络层.md`（连接数、QPS、P99、CPU、退出耗时、io_threads=1 与 =2 的对比）。

**Security/Boundary Notes:** 压测工具仅用于 localhost/内网，不做公网放大。

---

## gtest 清单

| 测试文件 | 用例 | 断言要点 |
|---|---|---|
| `test_smoke.cpp` | `Smoke.Builds` | 工具链可用 |
| `test_socket.cpp` | `NonBlockAndCloexecSet` | `O_NONBLOCK`、`FD_CLOEXEC` 位存在 |
| | `MoveTransfersOwnership` | 源 fd == -1，目标有效 |
| | `DestructorClosesFd` | 析构后 `fcntl(F_GETFD) == -1 && errno == EBADF` |
| | `GetSocketErrorOnReset` | 对端 `SO_LINGER{1,0}` 关闭后返回 `ECONNRESET` |
| `test_channel_poller.cpp` | `ReadableTriggersReadCallback` | callback 被调用一次，`revents & EPOLLIN` |
| | `DisableReadingStopsEvents` | 100ms poll 无事件 |
| | `RemoveChannelDetachesFd` | 移除后 poll 不再返回该 channel |
| `test_event_loop.cpp` | `RunInLoopSameThreadImmediate` | 返回前已执行 |
| | `QueueInLoopFromOtherThread` | 1000 次全部执行，计数 == 1000 |
| | `QuitWakesUpBlockedPoll` | < 200ms 返回 |
| | `RunInLoopInsideCallbackNoHang` | 嵌套投递不死循环 |
| `test_buffer.cpp` | `AppendRetrieve` | 内容与长度一致 |
| | `RetrievePartialLeavesRest` | 剩余可读字节正确 |
| | `GrowBeyondInitialCapacity` | 1MB 追加后内容完整 |
| | `ReadFdLoopsUntilEagain` | 一次调用读满 200KB |
| `test_read_eagain.cpp` | `SingleEventReadsUntilEagain` | 单次事件收满 1MB |
| | `HalfPacketKeptForNextEvent` | 半包留到下次，不误回调 |
| `test_write_path.cpp` | `SendPartialThenEpollout` | 分片补齐，总量一致 |
| | `SendFromOtherThreadDelivered` | 跨线程发送成功 |
| | `NoEpolloutWhenIdle` | 空闲 500ms 无写事件 |
| | `BackpressureStopsReading` | 超水位后停止读，回落恢复 |
| `test_tcp_server.cpp` | `AcceptThenEcho` | 回显内容一致 |
| | `PeerFinTriggersCloseCallback` | 关闭回调被调用一次 |
| | `ServerStopClosesAll` | 10 连接全部关闭 |
| `test_lifecycle.cpp` | `SharedPtrOutlivesClose` | 关闭后 `weak.lock()==nullptr` |
| | `DestroyNotInsideCallback` | 析构发生在回调返回之后 |
| | `NoDoubleClose` | 重复 `forceClose` 无崩溃、无二次 close |
| `test_timer_queue.cpp` | `RunAfterFiresOnce` | 恰好 1 次 |
| | `RunEveryFiresN` | 100ms 内 ≥ 3 次 |
| | `CancelBeforeFire` | 0 次 |
| | `CancelInsideCallback` | 自身取消不崩溃、不再触发 |
| | `SameDeadlineOrderBySeq` | 同刻按插入序 |
| | `CancelThenRecreateDoesNotFire` | seq 校验防误删 |
| | `PastDeadlineFiresImmediately` | 下一轮即触发 |
| | `TenThousandTimersAllFire` | 10000 个全部触发 |
| `test_shutdown.cpp` | `SigintQuitsWithinOneTick` | < 200ms 退出 |
| | `PendingWriteFlushedBeforeClose` | 客户端收满字节 |
| | `NoFdLeakAfterStop` | fd 差值 ≤ 2 |

**合计 39 个用例**（Phase 6 要求的 ≥60 例由本阶段 39 例 + Phase 2/4 补充，计划内不虚报）。

---

## 验证（Phase 3 DoD）

- [ ] `mkdir -p build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j4` 零警告
- [ ] `cd build && ctest --output-on-failure` 全绿（39 例）
- [ ] `io_threads=1` 与 `io_threads=2` 各跑一次 2000 连接 × 600s：失败 0，fd 稳定，CPU 正常
- [ ] 压测中 `SIGINT`：退出 < 5s
- [ ] `ENABLE_ASAN=ON` 下跑完整 ctest + 一次压测 + 一次退出：无 ASan/LSan 报告
- [ ] `grep -rn "\bdelete\b" server/net | grep -v "= delete"` 为空
- [ ] `grep -rn "volatile" server/net` 为空
- [ ] 所有 `CMakeLists.txt` 的 `cmake_minimum_required` ≤ 3.5，无 3.6+ 命令

---

## 风险与对策

| 风险 | 触发点 | 对策 / 检测 |
|---|---|---|
| ET 未读到 EAGAIN，连接假死 | Task 3 | `SingleEventReadsUntilEagain` 用 1MB 一次性写入；压测中查「连接数在、QPS 归零」 |
| ONESHOT 漏武装 | Task 3/5 | `handleEvent` 末尾统一 `rearm()`；调试期加「每事件 MOD 计数 == 1」断言 |
| `EPOLLOUT` 常驻导致 CPU 100% | Task 4 | `NoEpolloutWhenIdle`；验收查 `top -H` |
| 析构发生在回调栈 → 偶发 SIGSEGV | Task 5 | `tie(weak_ptr)` + `queueInLoop`；`DestroyNotInsideCallback` 断言时序；ASan 兜底 |
| fd 复用导致「幽灵事件」 | Task 5 | 关闭前 `EPOLL_CTL_DEL`；`destroyInLoop` 里先 remove 再 close |
| timerfd `ABSTIME` 时钟基准错位 | Task 6 | 构造时算 `delta`；`RunEveryFiresN` 断言 100ms 内 ≥3 次（错位会明显偏慢或永触发） |
| gtest 版本过高要求 C++14 | Task 0 | 锁 release-1.10.0；若编译报 `std::make_unique` 则是版本问题，立即降级 |
| cmake 3.5 不认新命令 | Task 0 | 只用 `add_test`；构建命令固定 `mkdir build && cd build && cmake ..`；CI 里加一条 3.5 的容器校验 |
| 多 sub Reactor 问题难复现 | Task 5 | `io_threads=1` 先全绿再开 2；日志带 `tid` |
| 压测环境噪声（虚拟机/宿主争抢） | Task 8 | 记录 `nproc`/负载；报告标注环境；同参数跑 3 次取中位 |
| 2C4G 上 2000 连接内存超预期 | Task 8 | 水位默认 8MB 偏大；压测时下调至 2MB 并记录 RSS |

---

## 回滚

- 本阶段新增目录（`server/net/`、`benchmarks/`）独立于业务，出问题**直接删除该目录 + 从根 CMakeLists 移除 add_subdirectory** 即可回到 Phase 2 状态。
- `TcpServer` 与 `echo_server` 无持久化副作用，回滚不涉及数据迁移。
- 若 `io_threads>1` 出现难以定位的竞态，**降级为 1 继续交付**（设计已保证行为等价），把多线程问题留到 Phase 7 用 TSan 专项处理。

## Deferred Decisions（不阻塞，已给安全默认）

| 决策 | 为何可延后 | 默认实现 |
|---|---|---|
| `io_threads` 生产默认值 | 需压测数据支撑 | 默认 2（`min(4, cpu)`），`-i 1` 可覆盖 |
| 发送缓冲高低水位 | 依赖 Phase 5 真实文件大小分布 | 8MB / 2MB，配置项预留 |
| 是否引入 TSan 常驻 | 会显著拖慢压测 | 仅 Phase 7 专项开启 |
| 业务线程池与文件 IO 池 | Phase 3 无业务，建了即空转 | **本阶段只留接口与配置项，不实现**；Phase 4/5 落地 |
| 日志接入 | Phase 2 异步日志若未就绪 | 先用 `fprintf(stderr)` 占位并集中在 `net/log.h` 单点，Phase 2 就绪后整体替换 |
