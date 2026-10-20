# cloud-media

基于 C++11 的云盘 + 在线音视频播放系统（单仓库）。

- 服务端：Linux + 自研 Reactor/epoll(ET) + 线程池 + MySQL/Redis
- 客户端：Windows + Qt 5.12 + 内嵌 FFmpeg 播放器（32 位）

## 当前状态（Phase 8 / P8-5）

五条主链路已打通并有联调证据：**登录 → 上传（含秒传/坏片重传）→ 断点续传 → 分享 → 边下边播**。
客户端可一键出独立发布包：`scripts/package_client.sh`（windeployqt + FFmpeg/SDL DLL → `deploy/`）。

## 快速开始

~~~bash
# 客户端构建（Qt 5.12.11 mingw73_32）
export PATH="/d/QT/Tools/mingw730_32/bin:/d/QT/5.12.11/mingw73_32/bin:$PATH"
cmake -S . -B build-client -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SERVER=OFF -DBUILD_CLIENT=ON -DCMAKE_PREFIX_PATH=D:/QT/5.12.11/mingw73_32 \
  -DCMAKE_CXX_COMPILER=D:/QT/Tools/mingw730_32/bin/g++.exe \
  -DCMAKE_C_COMPILER=D:/QT/Tools/mingw730_32/bin/gcc.exe
cmake --build build-client -j8

# 服务端（Linux，监听 8004）
cmake -S . -B build-on -DBUILD_SERVER=ON -DBUILD_CLIENT=OFF && cmake --build build-on -j8
./build-on/bin/cloud-media-server -c build/e2e.conf

# 客户端
build-client/bin/cloud-media-client.exe --host 192.168.142.131 --port 8004 \
  --tel 13900000011 --password-env CM_CLI_PASSWORD

# 全链路 demo（结尾 FULL_LINK_OK）/ 打包
scripts/e2e_p8.sh
scripts/package_client.sh --verify
~~~

细节见 `client/README.md`。

## 文档

- 架构设计：docs/specs/01-架构设计.md
- 协议与业务：docs/specs/06-业务设计.md
- 界面契约：docs/specs/08-界面设计契约.md
- 阶段计划与联调记录：docs/plans/phase-8.md
- 压测报告：docs/benchmark/压测报告.md
