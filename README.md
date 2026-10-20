# cloud-media

基于 C++11 的云盘 + 在线音视频播放系统（单仓库）。

- 服务端：Linux + 自研 Reactor/epoll(ET) + 线程池 + MySQL/Redis
- 客户端：Windows + Qt 5.12 + 内嵌 FFmpeg 播放器（32 位）

## 当前状态（Phase 9 / P9-2）

五条主链路打通并有联调证据：**登录 → 上传（秒传/坏片重传）→ 断点续传 → 分享 → 边下边播**。
服务端可用 Docker 一键起（mysql + redis + server）并用 CLI 冒烟；客户端可 windeployqt 独立发版。

## Docker Quick Start（新机器只需 git + docker）

~~~bash
git clone <repo-url> cloud-media && cd cloud-media
scripts/deploy.sh          # build → up → 等端口 → 上传/下载/MD5 冒烟 → DEPLOY_OK

# 或者手动：
docker compose up -d --build
docker compose ps
~~~

- 服务端监听 **8004**（宿主机端口冲突时改 `docker-compose.yml` 的 `ports`）。
- MySQL 首启动自动建库（`cloud_media`）、跑 schema + 补丁、建演示账号
  `13900000000 / demo123456`（**仅演示**，生产必须换）。
- 数据在 named volume：`mysql-data`（库）与 `cm-data`（`/data`：blobs / downloads / logs）。
- 构建 + 全量 ctest（含 MySQL/Redis 集成）：

~~~bash
docker compose --profile test run --rm build-test
~~~

- 冒烟用的 CLI 在服务端镜像里：`/app/bin/{upload_cli,download_cli,list_cli,recycle_cli}`，用法
  `docker compose exec server /app/bin/list_cli -h`。

## 本地构建（不用 Docker）

~~~bash
# 服务端（Linux，监听 8004）
cmake -S . -B build-on -DBUILD_SERVER=ON -DBUILD_CLIENT=OFF
cmake --build build-on -j8
./build-on/bin/cloud-media-server -c <server.ini>

# 客户端（Windows + Qt 5.12 mingw73_32；工具链路径见 docs/plans/phase-8.md）
cmake -S . -B build-client -G "MinGW Makefiles" \
  -DBUILD_SERVER=OFF -DBUILD_CLIENT=ON -DCMAKE_PREFIX_PATH=<QT_ROOT>
cmake --build build-client -j8
build-client/bin/cloud-media-client.exe --host <server-host> --port 8004 \
  --tel <手机号> --password-env CM_CLI_PASSWORD
~~~

客户端细节（依赖、打包）见 `client/README.md`；全链路 demo 与打包自检：

~~~bash
scripts/e2e_p8.sh              # 结尾 FULL_LINK_OK
scripts/package_client.sh --verify
~~~

## 文档

- 架构设计：docs/specs/01-架构设计.md
- 协议与业务：docs/specs/06-业务设计.md
- 界面契约：docs/specs/08-界面设计契约.md
- 播放器设计：docs/specs/09-播放器设计.md
- 阶段计划与联调记录：docs/plans/phase-8.md、docs/plans/phase-9.md
- 压测报告：docs/benchmark/压测报告.md
