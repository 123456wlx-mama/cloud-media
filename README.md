# cloud-media

基于 C++11 的云盘 + 在线音视频播放系统（单仓库）。

- 服务端：Linux + 自研 Reactor/epoll(ET) + 线程池 + MySQL/Redis
- 客户端：Windows + Qt 5.12 + 内嵌 FFmpeg 播放器
- 设计文档：docs/specs/01-架构设计.md

## 当前状态

Phase 1：仓库骨架 + CMake 分层 + 占位可执行文件。业务代码尚未开始。

## 构建

~~~bash
# Linux 服务端
cmake -S . -B build -DBUILD_SERVER=ON -DBUILD_CLIENT=OFF -DBUILD_TESTS=ON
cmake --build build -j8
./build/bin/cloud-media-server --version

# Windows 客户端（MinGW）
cmake -S . -B build -G "MinGW Makefiles" -DBUILD_SERVER=OFF -DBUILD_CLIENT=ON
cmake --build build -j8
build/bin/cloud-media-client.exe --version
~~~

## 目录

- common/：基础库、日志、配置、协议 v2
- server/：网络、连接池、DAO、缓存、业务、媒体
- client/：网络、传输、SQLite、播放器、UI
- legacy/：旧工程只读归档，不参与构建
- docs/：架构、协议、数据库、压测与面试材料
