# Phase 10 计划与联调记录

> 目标：新机器只装 git + docker，就能 `clone → compose up → 全链路可用`；同时 build+ctest 链路可复现。
> P10-1 交付 Dockerfile / docker-compose / deploy.sh / README Docker Quick Start。

## 1. 交付物

| 文件 | 作用 |
|---|---|
| `Dockerfile` | 多阶段：`build`（ubuntu:22.04 + cmake/g++/libmysqlclient-dev/libhiredis-dev）→ `runtime`（只装 libmysqlclient21 / libhiredis0.14） |
| `.dockerignore` | 把 .git / legacy / client / build* / deploy 排除出构建上下文 |
| `docker/server.conf` | 容器内配置：mysql.host=mysql、redis.host=redis、data_dir=/data（键名按 server_config.cpp） |
| `docker/entrypoint.sh` | 建 `/data/{blobs,downloads,logs}` 后 exec 服务端 |
| `docker/mysql/00-test-db.sql` | 建 `cloud_media_test` 库与 `cm_test` 用户（集成测试用） |
| `docker/mysql/30-demo-user.sql` | 演示账号 `13900000000 / demo123456`（MD5(salt+pwd)，仅演示） |
| `docker/mysql/40-test-schema.sh` | 把同一套 schema 应用到测试库 |
| `docker-compose.yml` | mysql:8.0 + redis:7-alpine + server；`build-test` 走 profile=test |
| `scripts/deploy.sh` | build → up → TCP 等端口 → 容器内 upload/download + MD5 → `DEPLOY_OK`；失败打印步骤/容器状态/日志 tail |

~~~text
        ┌────────────┐      ┌────────────┐
        │  mysql:8.0 │      │ redis:7    │   （initdb.d 按 00→40 顺序建库/建表/建演示号）
        └─────┬──────┘      └─────┬──────┘
              └──── healthy ──────┘
                       │ depends_on
                 ┌─────▼──────┐        ports 8004:8004
                 │   server   │◄────── curl / CLI / 客户端
                 │  /data 卷  │
                 └────────────┘
   build-test（profile=test）：同一 build 镜像，挂源码，跑 cmake+ctest（DB/Redis 全集成）
~~~

## 2. DoD 证据

**A. 干净目录 clone → compose up → 冒烟**

~~~text
$ git clone --depth 1 file:///d/cloud-media /d/p10-clean
$ cd /d/p10-clean && bash scripts/deploy.sh
== docker compose build        ... server 镜像构建成功
== docker compose up -d        ...
== wait server port
server 已监听 8004
== smoke: upload
UF_ID=1  FILE_MD5=91fafb0ffff3c73a324a42be894da04e
== smoke: locate ufId          UF_ID=1
== smoke: download + md5
LOCAL_MD5=91fafb0ffff3c73a324a42be894da04e
REMOTE_MD5=91fafb0ffff3c73a324a42be894da04e
== container status
p10-clean-mysql-1   Up 3 minutes (healthy)   3306/tcp
p10-clean-redis-1   Up 3 minutes (healthy)   6379/tcp
p10-clean-server-1  Up 3 minutes (healthy)   0.0.0.0:8004->8004/tcp
DEPLOY_OK  (server: http://127.0.0.1:8004, demo tel=13900000000)
~~~

> 说明：本批未提交，所以 "干净 clone" = `git clone` 仓库 HEAD + 覆盖本批新增的 Docker 文件；
> 这样验证的是"仓库可克隆 + 发布树自洽"，不依赖本机编译产物。

**B. build + ctest（含 MySQL/Redis 集成）**

~~~text
$ docker compose --profile test run --rm build-test
...
100% tests passed, 0 tests failed out of 43
Total Test time (real) =  80.42 sec
~~~

口径：Linux 容器内、BUILD_SERVER+BUILD_STORAGE+BUILD_TESTS+ENABLE_DB_INTEGRATION_TESTS=ON、
BUILD_CLIENT=OFF（客户端是 Windows Qt，不进 Linux 镜像）。43 项里
`mysql_pool_integration / redis_pool_integration / storage_integration` 全部真实执行（无 SKIP）。

**C. deploy.sh 一次成型**

本机连续跑 3 次：前两次暴露脚本 bug（见 §3），修完后第 3 次直接 `DEPLOY_OK`；
在干净 clone 目录再跑一次仍 `DEPLOY_OK`。

## 3. 踩到的坑（都会复现）

1. **Git Bash 里 docker 找不到 compose / 凭据**：PATH 里没有
   `%LOCALAPPDATA%\Programs\DockerDesktop\resources\bin` 时，`docker compose` 变成未知命令；
   强行覆盖 `DOCKER_CONFIG` 又会把 compose 插件目录一起换掉。结论：Windows 上用 PowerShell/cmd，
   或在 Git Bash 里把该目录加进 PATH。
2. **MSYS 路径转换**：`docker compose exec ... bash -c '/app/bin/list_cli ...'` 会被 Git Bash 把
   `/app/bin/...` 转成 `C:/Program Files/Git/app/bin/...`（报 `C:/Program: No such file`）。
   修法：命令串用 `set -e; ...` 开头（不以 / 开头）或加 `MSYS_NO_PATHCONV=1`。
3. **compose 折叠标量拆行**：`command: >` 里的多行 cmake 命令被拆成多条 shell 命令，
   报 `-DBUILD_SERVER=ON: command not found`。修法：命令写成单行字符串列表。
4. **宿主端口冲突**：VM 上已有 8004 时，改 `docker-compose.yml` 的 `ports`（如 18004:8004）。

## 4. 遗留

| 项 | 状态 | 说明 |
|---|---|---|
| GitHub 远端 clone 验证 | 待做（P10-2） | 本批用本地 `file://` clone 模拟；网络恢复后补一次远端 clone 截图 |
| README 「5 分钟跑起来」完善 | P10-2 | 本批先给 Docker Quick Start 与通用命令 |
| 镜像瘦身 | 可选 | runtime 约 200MB（ubuntu 基础层）；可换 distroless/alpine 但需自编 mysqlclient |
| 数据库初始化幂等 | 已知 | initdb.d 只在空数据卷执行；改动 schema 后需 `down -v` |
