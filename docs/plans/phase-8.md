# Phase 8 实施计划（Qt 5.12 客户端：三段式 + 传输 + 播放）

> 对应《终端改造路线图》Phase 8。本文件是计划与任务分解；P8-0 产出见
> `docs/specs/08-界面设计契约.md`（设计契约）、`docs/screenshots/p8-0-skeleton.png`（骨架截图）。

## Overview

把 Phase 1 的占位 exe（只打印版本）升级为可用的 Windows 客户端：**登录 → 文件列表 → 上传/下载（断点续传）→ 分享 → 在线播放**，
界面按"文件区 / 传输区 / 播放区"三段式组织，深色主题，纯 C++11，UI 层不碰网络与磁盘细节。

## DoD（P8 全部收口时）

| 指标 | 目标 |
|---|---|
| 功能 | 上述 5 条主链路在本机 8004 服务端上全通；断点续传中断后能续 |
| 构建 | `build-client` 零告警（`-Wall -Wextra -Wpedantic`），Release 产物可双击运行（DLL 随包） |
| 依赖红线 | `client/` 无 `#include "server/`；`common/` 无 Qt 符号 |
| 规范 | 纯 C++11（禁 make_unique/optional/string_view/filesystem/泛型 lambda） |
| 视觉 | 颜色/字号/间距全部来自 `08-界面设计契约.md` §4，QSS 单文件 |
| 播放 | 边下边播（首块 4MB 起播）+ 基础 HLS（只 remux 不转码） |

## 环境锁定（32 位一致性约束）

Qt 5.12.11 的 `mingw73_32` 是 **32 位**，必须与后续 FFmpeg/SDL 二进制保持一致；混用 64 位会在 P8-5 链接期炸。

~~~bash
# 1) 编译器等 DLL 要先在自己的 PATH 里（否则 g++ 静默失败：rc=1 且无报错）
export PATH="/d/QT/Tools/mingw730_32/bin:/d/QT/5.12.11/mingw73_32/bin:$PATH"

# 2) 配置（独立目录，不污染 build/）
cmake -S . -B build-client -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SERVER=OFF -DBUILD_CLIENT=ON \
  -DCMAKE_PREFIX_PATH=D:/QT/5.12.11/mingw73_32 \
  -DCMAKE_CXX_COMPILER=D:/QT/Tools/mingw730_32/bin/g++.exe \
  -DCMAKE_C_COMPILER=D:/QT/Tools/mingw730_32/bin/gcc.exe

cmake --build build-client -j8
build-client/bin/cloud-media-client.exe --version   # → cloud-media-client 0.1.0 (protocol v2)
~~~

**运行期**必须把这两个目录加进 PATH（或把 Qt DLL 拷到 exe 旁）：
`D:\QT\5.12.11\mingw73_32\bin`、`D:\QT\Tools\mingw730_32\bin`。

| 约束 | 说明 |
|---|---|
| 编译器 | 固定 Qt 自带 `g++ 7.3.0 (i686)`；仓库默认工具链与 Qt 不匹配，**不要用 PATH 里的 g++** |
| 位宽 | 32 位（mingw73_32）。换 64 位 Qt 必须同步换 64 位 FFmpeg/SDL |
| CMake | 本机 4.3.3 可跑 `cmake_minimum_required(3.5)`（仅告警）；CI 仍是 3.5 语法红线 |
| 已知坑 | 仓库里部分文件带 append-only 属性（`lsattr` 显示 `a`），原地改写会**静默失败**；遇到"写了没生效"先 `mv` 掉再写 |
| 已知坑 | `cmake -G "MinGW Makefiles"` 探测编译器时需要 PATH 含 MinGW bin，否则报 "compiler test failed" 且不给原因 |

## Files and Responsibilities

| 文件 | 职责 |
|---|---|
| `client/app/main.cpp` | 入口：`--version` / `--ui-demo` / `--screenshot`；加载 QSS 与字体 |
| `client/ui/MainWindow.{h,cpp}` | 三段式主窗口（QSplitter + 分区头 + 空态），P8-2 起接 Model |
| `client/ui/LoginDialog.{h,cpp}` | 登录对话框骨架（本地校验）；P8-1 换 LoginRq |
| `client/ui/app.qss` + `resources.qrc` | 深色主题基线（色板与契约 §4 一一对应） |
| `client/netapi/` | QTcpSocket 适配 + 协议 v2 收发 + 独立网络线程（P8-1） |
| `client/transfer/` | 上传/下载任务、队列、断点续传状态（P8-3） |
| `client/sqlapi/` | SQLite 本地任务缓存（P8-3） |
| `client/player/` | PlayerCore、解封装、解码、渲染接口（P8-5） |
| `docs/specs/08-界面设计契约.md` | 信息架构/状态/色板/组件/legacy 对照（P8-0 产出） |

## 联调记录（P8-1）

| 项 | 值 |
|---|---|
| **VM IP（DHCP，会变）** | **192.168.142.131**（2026-10-20 实测，用 _vm.py run "hostname -I" 复查） |
| 服务端 | 8004（e2e 那套：build-on/bin/cloud-media-server -c build/e2e.conf，库 cloud_media_test） |
| 测试账号 | 13900000011 / p81（一次性临时账号，u_name=p81；脚本 build-client/add_p81_user.sh） |
| 启动命令 | cloud-media-client.exe --host 192.168.142.131 --port 8004 --tel 13900000011 --password-env CM_CLI_PASSWORD |
| 无界面取证 | 同上再加 --smoke（登录+列表打印后退出，退出码 0 成功） |
| 界面取证 | 同上再加 --screenshot docs/screenshots/p8-1-login-list.png（2.5s 后自动截图退出） |

证据（smoke 输出）：

~~~text
LINK state=1 已连接
LOGIN_OK uid=6679 name=p81
LIST code=0 rows=2
ROW p81_demo.mp4 size=2097152 dir=0
ROW p81_report.pdf size=1048576 dir=0
SMOKE_OK
~~~

### 联调踩到的三个坑

1. **Windows 系统代理会打断 QTcpSocket**：不改的话 connectToHost 直接失败，errorString 是
   "The proxy type is invalid for this operation"，重连 5 次全失败。修法：socket->setProxy(QNetworkProxy::NoProxy)。
2. **ctest 继承的 PATH 里可能有 64 位 MinGW**（cmake 自带那套），32 位测试 exe 会加载到 64 位
   libstdc++-6.dll → 0xc000007b。修法：测试属性里按 CMAKE_CXX_COMPILER 目录显式设 PATH。
3. **未连接时写 socket 会丢帧**：登录往往在 Connecting 阶段就发出，QTcpSocket::write 此时无效。
   修法：worker 里加 outbox，connected() 后一次性 flush。

## 联调记录（P8-2 断线→暂停 + 只补缺块）

**根因**：`NetClient::onRequestFailed` 对每条 worker 失败同时发 `requestTimeout(seq)` 与
`requestFailed(type)`；断线时 `failAll` 先到、`linkState(2)` 后到，TransferQueue 处理超时时
`m_linkUp` 仍是 true，于是把"连接断开"钉成 Failed。

**修法**（同一 seq 只允许一条终态路径，断线优先）：

1. NetWorker 的 `Pending` 加 `terminated`，`takePending()` 统一做"取出+标记+删除"；超时/收响应/
   断线三条终态路径都先判它，重复到达直接丢弃。
2. 断线与超时拆成两个信号：`requestAborted(seq,type,reason)`（连接断开）与
   `requestTimeout(seq,type)`（链路正常但 5s 无响应）。`onTick` 发现 socket 非 Connected 时走
   aborted，不再判超时。
3. TransferQueue：`onAborted` → 运行中任务转 Paused 并保留 `.part/.part.state`；`onTimeout`
   在 `!m_linkUp` 时同样转 Paused，只有链路正常才 Failed（可重试）。
4. 服务端 ErrorRs 改为原样抛给传输层（之前被吞成"请求超时"，真实错误码丢失）。

**证据**（真实 20MB，VM 192.168.142.131:8004，下到 1/2 处 `fuser -k 8004/tcp` 再拉起）：

~~~text
TASK 1 download state=传输中 bytes=4194304/20971520 blocks=1/5 fetched=1
TASK 1 download state=已暂停 bytes=12582912/20971520 blocks=3/5 fetched=3
NOTICE ERR smoke_download.bin 已暂停：连接已断开
LINK state=2 detail=重连中(1/5) The remote host closed the connection
LINK state=2 detail=重连中(2/5) Connection refused
LINK state=1 detail=已连接
LOGIN_OK uid=6679 name=p81
RESUME task=1 bytesDone=12582912 blocks=3/5
NOTICE OK smoke_download.bin 续传：已下 3/5 块（12582912 字节），本次只取 2 块
TASK 1 download state=传输中 bytes=16777216/20971520 blocks=4/5 fetched=1
NOTICE OK smoke_download.bin 已完成
TASK 1 download state=已完成 bytes=20971520/20971520 blocks=5/5 fetched=2
~~~

落盘 20971520 字节，`.part` / `.part.state` 已清理；`cm-tasks.db` 已加入 .gitignore。

## Tasks

| # | 任务 | 验收 | 状态 |
|---|---|---|---|
| **T8-0** | 环境锁定 + 设计契约 + 三段式骨架 | 零告警构建、`--version` 输出 v2、骨架截图、红线 grep | **本批完成** |
| T8-1 | netapi：QTcpSocket + 网络线程 + LoginRq/ListFileRq | 登录/列表在真服务端跑通，错误码有文案 | **本批完成** |
| T8-2 | 文件区：QAbstractTableModel + 面包屑 + 右键菜单 + 拖拽上传 | 1000 行列表滚动不卡，排序/分页走协议 | 待做 |
| T8-3 | 传输区：上传/下载队列、进度、暂停/取消、断点续传 | 中断后重启可续；限速 2MB/s 时速度显示正确 | 待做 |
| T8-4 | 分享：生成/提取/过期提示 | 分享码 + 提取码全流程 | 待做 |
| T8-5 | 播放器：FFmpeg 解码 + 边下边播 + 基础 HLS | 10MB 视频首块起播 < 1.5s | 待做 |
| T8-6 | 主题打磨 + 打包（windeployqt） | 双击可运行，DLL 齐全 | 待做 |

## 风险

1. **32 位 FFmpeg**：legacy 的 ffmpeg/SDL 是 32 位，新客户端必须一致；P8-5 前先验证 `dumpbin /headers`。
2. **UI 线程阻塞**：所有网络/磁盘必须离开 UI 线程，否则 1000 条任务时卡死（契约 §1.3 单向数据流）。
3. **Model/View 与 QTableWidget 混用**：legacy 的 item 模式禁止带入，否则性能回退到 O(n) 刷新。
4. **QSS 颜色漂移**：颜色只允许来自契约 §4.1，评审时先看 `app.qss` 里有没有硬编码新色值。
