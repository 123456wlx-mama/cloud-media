# Phase 6 覆盖率基线（T6-1，2026-10-19）

## 环境与工具

| 项 | 值 |
|---|---|
| OS / 编译器 | Ubuntu 16.04 / g++ 5.4.0 |
| CMake | 3.5.1 |
| 覆盖率工具 | lcov 1.12（apt 安装，含 genhtml） |
| 构建目录 | `build-cov`（`-DENABLE_COVERAGE=ON`） |
| 测试环境 | `CM_TEST_MYSQL=1 CM_TEST_REDIS=1`，库 `cloud_media_test` |

构建与采集命令：

~~~bash
mkdir -p build-cov && cd build-cov
cmake .. -DBUILD_SERVER=ON -DBUILD_STORAGE=ON -DBUILD_TESTS=ON -DENABLE_COVERAGE=ON
make -j4
CM_TEST_MYSQL=1 CM_TEST_MYSQL_USER=cm_test CM_TEST_MYSQL_PASSWORD=*** CM_TEST_REDIS=1 \
  ctest --output-on-failure          # 41/41 Passed，30.60s

cd ~/cloud-media
lcov --capture --directory build-cov --output-file build-cov/coverage.info
lcov --extract build-cov/coverage.info '/home/colin/cloud-media/common/*' -o build-cov/common.info
lcov --extract build-cov/coverage.info '/home/colin/cloud-media/server/net/*' -o build-cov/net.info
genhtml build-cov/common.info -o build-cov/coverage_html_common
genhtml build-cov/net.info -o build-cov/coverage_html_net
~~~

> lcov 1.12 的 `--no-external` 在本布局下会把项目源文件全部判为 external（capture 结果只剩 `TN:`）。
> 因此改为「先全量 capture，再按绝对路径 extract」，等效且可用。

## 总覆盖率（DoD 对照）

| 范围 | 行覆盖 | DoD | 结论 |
|---|---|---|---|
| `common/` | **89.0%**（2978 / 3346 行） | 80% | 通过（+9.0pp） |
| `server/net/` | **83.1%**（1133 / 1363 行） | 60% | 通过（+23.1pp） |

函数覆盖：common 97.8%（696/712）、server/net 90.1%（282/313）。
分支覆盖：lcov 报 `no data found`（当前 `-fprofile-arcs` 未采集分支，仅行/函数）。

lcov 原文：

~~~text
# common
Summary coverage rate:
  lines......: 89.0% (2978 of 3346 lines)
  functions..: 97.8% (696 of 712 functions)
  branches...: no data found

# server/net
Summary coverage rate:
  lines......: 83.1% (1133 of 1363 lines)
  functions..: 90.1% (282 of 313 functions)
  branches...: no data found
~~~

HTML 报告：`build-cov/coverage_html_common/index.html`、`build-cov/coverage_html_net/index.html`。

## 覆盖率最低的 15 个生产源文件

（只统计 `.cpp`，排除 `tests/` 与 `test_*`；数值来自 lcov LH/LF。）

| # | 文件 | 行覆盖 | 未覆盖/总行 | 归类 |
|---|---|---|---|---|
| 1 | common/base/error_code.cpp | 55.6% | 28/63 | **测试盲区**：错误码映射表只测了常用项 |
| 2 | server/net/Poller.cpp | 57.1% | 3/7 | **不可测为主**：抽象基类/工厂平台分支 |
| 3 | server/net/TcpServer.cpp | 58.9% | 23/56 | **测试盲区**：start/stop/shutdownAll 生命周期未覆盖 |
| 4 | server/net/Socket.cpp | 60.7% | 11/28 | **不可测为主**：bind/listen 系统调用失败分支 |
| 5 | server/net/TcpConnection.cpp | 65.2% | 101/290 | 混合：sendFile/RST/EPIPE 错误路径（资源）+ 关闭状态机（盲区） |
| 6 | common/config/server_config.cpp | 72.8% | 34/125 | **测试盲区**：配置校验失败分支 |
| 7 | server/net/handler_registry.cpp | 74.7% | 21/83 | **测试盲区**：准入/释放/停机 drain 路径 |
| 8 | common/base/buffer.cpp | 79.1% | 23/110 | 混合：readFd errno（平台）+ ensureWritable 边界（盲区） |
| 9 | server/net/Acceptor.cpp | 79.4% | 14/68 | **不可测为主**：accept EMFILE/ECONNABORTED |
| 10 | common/config/ini_config.cpp | 80.9% | 22/115 | **测试盲区**：重复键/缺段/非法行 |
| 11 | common/protocol/codec.cpp | 83.9% | 18/112 | **测试盲区**：decodeBulk 边界（半帧/超长/坏类型） |
| 12 | common/log/logger.cpp | 84.2% | 44/278 | 混合：磁盘写失败/滚动（资源）+ 级别过滤（盲区） |
| 13 | server/net/EpollPoller.cpp | 86.8% | 10/76 | **不可测为主**：epoll_ctl 异常分支 |
| 14 | server/net/TimerQueue.cpp | 89.4% | 11/104 | 混合：timerfd 边界（平台）+ 取消/重排（盲区） |
| 15 | server/net/Channel.cpp | 89.7% | 6/58 | **测试盲区**：事件位组合（EPOLLIN/EPOLLOUT/HUP） |

说明：`common/` 与 `server/net/` 合计只有 21 个生产 `.cpp` 有数据；其余文件（含头文件）多为 100% 或不在本次统计范围。

## 建议补测优先级 Top 10

1. **error_code.cpp 映射表**：对每个 `ErrorCode`/`ProtocolError` 断言 `to_string` 与 `is_retryable`，纯函数、成本最低、收益最大。
2. **server_config.cpp 校验分支**：缺 key、越界端口、空 data_dir、非法 log level、密码脱敏。
3. **ini_config.cpp 解析边界**：注释/空行/重复键/缺段/CRLF。
4. **handler_registry.cpp**：准入拒绝、释放回调、stop 时 drain、重复注册、未知类型。
5. **TcpServer.cpp**：start/stop/shutdownAll、端口占用、重复 start、accept 循环退出。
6. **TcpConnection.cpp 关闭/错误状态机**：对端 RST、半关闭、send 失败、sendFile 中途断开。
7. **codec.cpp decodeBulk**：半帧、恰好 19B、超 `kMaxPayload`、坏 magic/version、10006 之外的类型。
8. **buffer.cpp**：`ensureWritable` 扩容/compact、`readFd` 的 EINTR/EAGAIN、超 `kMaxBytes`。
9. **Socket/Acceptor 故障注入**：用无效地址/已占用端口触发 bind/listen 失败；accept EMFILE 可用注入 hook。
10. **logger.cpp**：级别过滤、滚动（max_size/roll_days）、写失败降级、shutdown flush。

前 4 项都属于纯逻辑/配置层，不依赖网络与 DB，适合作为 T6-2 的第一批。

## 备注

- 本轮只采基线，未补任何测试、未改代码。
- `ctest` 全量 41/41 通过；覆盖数据来自同一轮执行。
- 覆盖率数字会随测试顺序与 DB/Redis 状态轻微波动（例如 timers 用例的真实等待），基线以本次报告为准。
