# Phase 5 缓存降级演练（E-4，2026-10-19）

## 环境

| 项 | 值 |
|---|---|
| OS / 编译器 | Ubuntu 16.04 / g++ 5.4.0 |
| MySQL | 5.7.33，库 `cloud_media_test`，账号 `cm_test` |
| Redis | 本机 6379（`/etc/redis/redis.conf`，无密码，db 0） |
| 服务端 | `build/bin/cloud-media-server -c build/e2e.conf`，127.0.0.1:8004 |
| 账号 | `13800000001`（algo=1），口令只从 `CM_CLI_PASSWORD` 读取 |
| 演练脚本 | `scripts/degrade_drill.sh`（`set -euo pipefail`，密码/库密码/sudo 密码均走环境） |

复跑：

~~~bash
. ~/.cm_e2e_env            # CM_CLI_PASSWORD
export CM_MYSQL_PASSWORD=... CM_SUDO_PASSWORD=...
cd ~/cloud-media && bash scripts/degrade_drill.sh
~~~

本轮结果：`ALL PASSED (safety assertions; see GAP lines)`，退出码 0；MySQL 恢复耗时 **90ms**。

## 一、停 Redis

| 场景 | 期望（06 §10） | 实际 | 判定 |
|---|---|---|---|
| 登录 | fail-closed 4007 | list_cli 登录成功（exit 0） | **偏差**：当前是连接级 uid，无 Redis session |
| 上传（秒传） | 回源 MySQL，可用 | exit 0，`STATUS=INSTANT_HIT` | 符合 |
| 下载打开 `--uf-id` | 4005/4007 | exit 4，`code=4005` | 符合，fail-closed |
| 分享 + 提取码 | 4008 | exit 4，`CODE=9003` | **GAP**：10022 未注册 handler |
| 分享无提取码 | 元信息回源成功，下载 fail-closed | exit 4，`CODE=9003` | **GAP**：10022 未注册，未走到 10010 |
| 列表 | 可用（不缓存） | exit 0，返回条目与 TOTAL_COUNT | 符合 |

## 二、停 MySQL

| 场景 | 期望 | 实际 | 判定 |
|---|---|---|---|
| 登录 | 4001/4002 | exit 2，`code=4001` | 符合（错误码），CLI 退出码是 2 不是 4 |
| 上传 init | 4001/4002 | 登录阶段即失败 `code=4001` | **未到达 10004**（鉴权依赖 MySQL） |
| 下载打开 | 4001/4002 | 登录阶段即失败 `code=4001` | **未到达 10010** |
| 列表 | 4001/4002，绝不给空列表 | exit 2，`code=4001`，输出无 `HAS_MORE`/`TOTAL_COUNT` | 安全属性符合；未到达 10008 |

核心断言成立：MySQL 不可用时 **没有任何空列表/假成功**。

## 三、恢复

- `service mysql start` 后轮询 `list_cli`：**90ms** 首次成功（要求 ≤5s），连接池半开探测自愈。
- Redis 在同一轮演练中 `shutdown nosave` 后通过 `sudo redis-server /etc/redis/redis.conf --daemonize yes` 恢复。

## 四、与 06 §10 的差异（按代码实现记录，未改鉴权方案）

1. **登录不依赖 Redis session**：实际鉴权是连接级 `ConnState.uid`，所以 Redis 挂时登录成功；06 §10.1 的 `sess:{token}` fail-closed 4007 尚未实现。
2. **10022 ShareOpen 未在 `server/app/main.cpp` 注册**：handler_registry 回 `9003 UnknownType`，与 Redis 状态无关。因此“提取码 fail-closed 4008”和“无提取码回源成功”两条都无法观测。
3. **10010 不消费 share_ticket**：`DownloadService::openDownload` 只有 `ufId` 路径（`rq.ufId == 0` 直接 1002），`DownloadRangeRq/shareTicket` 未参与；即使 10022 接上，分享下载（ufId=0）也会 1002，而不是 4005/4007。
4. **MySQL 挂时所有业务都停在登录**：UploadInit/DownloadOpen/ListFile 的 4001/4002 分支无法单独触发；`list_cli` 退出 2 而非 4（`connectAndLogin` 把登录拒绝映射为 2）。
5. 恢复耗时 90ms，满足 5s 目标。

## 五、结论

- **fail-closed 生效**：下载票据（4005）、连接鉴权（MySQL 不可用 4001）、分享路径（当前 9003 拒绝）。
- **fail-open 符合预期**：秒传回源 MySQL、列表、登录（连接级 uid）。
- **无越权**：Redis 挂时分享/下载均未放行；MySQL 挂时未返回空列表或假成功。
- **待修复（不在 E-4 内改）**：注册 10018/10022/10042 handler；让 10010 支持 shareTicket；再复跑本脚本，才能与 06 §10 的 4008/4005 对照完整。

## 六、演练期间的脚本修正

首轮 `run_capture` 会打印完整命令行，导致测试账号口令进入终端输出。已改为只打印程序名；演练结束后已轮换测试账号口令（`~/.cm_e2e_env` 与 `t_user` 同步更新），后续脚本不再回显参数。
