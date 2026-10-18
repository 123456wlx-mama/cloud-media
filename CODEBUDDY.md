# CODEBUDDY.md

This file provides guidance to CodeBuddy Code when working with code in this repository.

## 项目与当前状态

cloud-media：基于 C++11 的云盘 + 在线音视频播放系统（单仓库）。目标架构是 Linux 自研 Reactor/epoll(ET) + 线程池 + MySQL/Redis 服务端，Windows Qt 5.12 + 内嵌 FFmpeg 播放器客户端。

**当前是 Phase 1 骨架，不是完整实现：**

- 已建立：目录结构、CMake 分层、legacy 本地归档（Git 只跟踪源文件）、C++11 规范、Windows 客户端占位编译通过。
- 现有源码只有：**common/base/version.cpp**、**common/protocol/version.cpp**、**server/app/main.cpp**、**client/app/main.cpp** 及对应头文件。
- 尚未实现：日志、协议 Codec、配置解析、MySQL/Redis、网络框架、传输业务、播放器。
- 尚未完成：README 架构图、CI 骨架、Phase 1 全量验收。
- 测试现状：BUILD_TESTS 默认 ON，但没有任何 test target，ctest 目前 0 个用例；Phase 2 才引入 GoogleTest。

## 快速构建

### Windows 客户端（当前已验证）

~~~bash
cmake -S . -B build -G "MinGW Makefiles" -DBUILD_SERVER=OFF -DBUILD_CLIENT=ON
cmake --build build -j8
build/bin/cloud-media-client.exe --version
~~~

预期输出：cloud-media-client 0.1.0 (protocol v2)。

### Linux 服务端

~~~bash
cmake -S . -B build -DBUILD_SERVER=ON -DBUILD_CLIENT=OFF
cmake --build build -j8
./build/bin/cloud-media-server --version
~~~

预期输出：cloud-media-server 0.1.0 (protocol v2)。Windows 上不能构建服务端；CMake 会直接报错并提示使用 WSL/Linux。

### Docker 冒烟（本机无 Linux 时）

~~~bash
docker run --rm -v D:/cloud-media:/src -w /src ubuntu:22.04 bash -lc "apt-get update -qq && apt-get install -y -qq cmake g++ && cmake -S . -B build-linux -DBUILD_SERVER=ON -DBUILD_CLIENT=OFF && cmake --build build-linux -j4 && ./build-linux/bin/cloud-media-server --version"
~~~

若 Docker 无法挂载 D:/cloud-media，改用 //d/cloud-media 重试。

## 构建选项

| 选项 | 默认值 | 说明 |
|---|---|---|
| BUILD_SERVER | Linux=ON，Windows=OFF | 构建服务端；Windows 下强制 OFF，否则 FATAL |
| BUILD_CLIENT | Windows=ON，Linux=OFF | 构建客户端；Linux 默认 OFF |
| BUILD_TESTS | ON | 目前没有 test target，ctest 为空；Phase 2 起生效 |
| ENABLE_ASAN | OFF | Debug 下开 ASan/UBSan；仅 GCC/Clang |
| ENABLE_TSAN | OFF | 与 ENABLE_ASAN 互斥 |
| ENABLE_COVERAGE | OFF | gcov/--coverage；仅 GCC/Clang |
| USE_BUNDLED_THIRDPARTY | Windows=ON | 预留；当前 thirdparty 为空 |

## CMake Target 与依赖方向

~~~text
cm_compile_options (INTERFACE)
  └─ cm_common (STATIC, base/version.cpp) ── Threads
       └─ cm_protocol (STATIC, protocol/version.cpp)
            ├─ cloud-media-server (EXE, server/app/main.cpp)
            └─ cloud-media-client (EXE, client/app/main.cpp)
~~~

- 产物：build/bin/cloud-media-server、build/bin/cloud-media-client.exe、build/common/libcm_common.a、build/common/libcm_protocol.a。
- 后续 Phase 2~9 计划新增 cm_server_core、cm_client_net、cm_client_store、cm_player_core、cm_client_ui 等 target；在对应目录的 CMakeLists.txt 中添加。
- 依赖红线：common/protocol 不得出现 Qt、FFmpeg、socket；server 不得出现 Qt；client 不得 include server/；ui 不得直接 include FFmpeg/SDL。

## 目录与代码归属

| 需求 | 放置位置 |
|---|---|
| 错误码、Buffer、CRC32、字节序、时间工具 | common/base/ |
| 异步日志、日志级别、traceId | common/log/ |
| INI 配置、服务端/客户端配置结构 | common/config/ |
| 协议帧、消息类型、序列化、Codec | common/protocol/ |
| Reactor/epoll/连接/定时器 | server/net/ |
| 线程池、MySQL 池、Redis 池 | server/pool/ |
| DAO 与 SQL | server/db/ |
| 会话、秒传索引、分享码、令牌桶 | server/cache/ |
| 业务 Service 与协议 handler | server/logic/ |
| HLS Remux（只切片不重编码） | server/media/ |
| 客户端网络抽象与 QTcpSocket 适配 | client/netapi/ |
| 上传/下载任务、队列、续传状态 | client/transfer/ |
| SQLite 本地任务缓存 | client/sqlapi/ |
| PlayerCore、解封装、解码、渲染接口 | client/player/ |
| 对话框、Model/View、传输中心 | client/ui/ |
| 压测工具 | benchmarks/ |
| 架构/协议/数据库/压测文档 | docs/ |

## 测试、Lint 与验证

- 当前没有单元测试，也没有 clang-format/clang-tidy 配置；编译警告（-Wall -Wextra -Wpedantic）是唯一自动门槛。
- Phase 2 引入 GoogleTest 1.12.1 后，使用：

~~~bash
ctest --test-dir build --output-on-failure
ctest --test-dir build -R cm_common_tests --output-on-failure
~~~

- 每次改动后至少执行：

~~~bash
cmake --build build -j8
build/bin/cloud-media-client.exe --version
git diff --check
~~~

- 依赖红线检查：

~~~bash
grep -RIn "QObject\|QString\|#include <Q" common/ || echo clean-qt
grep -RIn "libav\|SDL_" common/ || echo clean-media
grep -RIn "#include \"server/" client/ || echo clean-server-dep
~~~

## 架构硬约束（来自 docs/specs/01-架构设计.md 与需求冻结）

- 纯 C++11：禁用 std::make_unique、std::optional、std::string_view、std::filesystem、泛型 lambda。
- 协议 v2：魔数 + 版本 + 类型 + SEQ + LEN + CRC32；强制升级，不实现 v1 解码。
- 文件存储：内容寻址 data/blobs/aa/bb/{md5}，t_file 对 MD5 唯一，引用计数在事务内维护。
- 传输状态：落 MySQL t_transfer_task/t_chunk；Redis 只做会话、秒传索引、分享码、限流。
- 客户端网络：QTcpSocket + 独立网络线程；不再使用 WinSock/HANDLE。
- 播放：边下边播 + 基础 HLS；HLS 只做 FFmpeg Remux（-c copy），只提供原画，不做多清晰度转码。
- 回收站：只软删 is_deleted，不做保留期、定时物理清理。
- 批量下载：客户端多选入队、有限并发；服务端只有单文件接口，不做 zip。
- 下载：4MB 分块、默认 4 连接、服务端令牌桶限速、sendfile 零拷贝。
- 并发：绝不在 Reactor/IO 线程做磁盘、数据库、日志同步写等阻塞操作。

## legacy 规则

- legacy/wanpan-dome 与 legacy/MediaPlayer 采用本地归档策略：Git 只跟踪源文件，二进制和第三方库不入库；本地需完整复制（wanpan-dome 含 .git 与 .pro.user，MediaPlayer 含 ffmpeg/SDL 二进制与 .pro.user）。
- 不修改 legacy/ 下任何文件；不参与 CMake 构建。
- 不要对 legacy/ 执行 git add/commit：wanpan-dome 内含 .git，会被 Git 当作嵌套仓库。
- 校验原样：

~~~bash
diff -rq d:/wanpan--dome legacy/wanpan-dome
diff -rq D:/mediaplayer/MediaPlayer legacy/MediaPlayer
~~~

两条命令均无输出、退出码 0 才算原样。

## 文档索引

- 架构设计：docs/specs/01-架构设计.md
- Phase 1 计划（计划，不是完成记录）：docs/plans/phase-1.md
- C++11 规范：.codebuddy/rules/cpp11-style.md
- 需求冻结（仓库外，换机器需同步）：D:/面试2026/网盘+播放视频改造项目/research/05-我们做什么.md
- 交互流程（仓库外）：D:/面试2026/网盘+播放视频改造项目/research/02-交互流程.md
- 术语与文案（仓库外）：D:/面试2026/网盘+播放视频改造项目/research/03-术语与文案.md
- 改造路线图（仓库外）：D:/面试2026/网盘+播放视频改造项目/终端改造路线图.md

## 非目标

以下功能已明确不做，不要擅自新增：多端同步、Office/PDF 在线预览、会员体系/付费容量、团队空间/协作权限、离线下载、多清晰度重编码转码、AI 摘要；播放器不做字幕、弹幕、投屏、画中画、选集、循环连播；服务端不做 zip 打包。

## 提交与阶段

- 每个阶段一个提交；提交前完成构建、测试（有测试后）、依赖红线检查。
- 当前 Phase 1 收尾项：README 架构图、CI、全量验收。
- 下一阶段 Phase 2：异步日志 + 协议 v2 + 配置 + 错误码。
