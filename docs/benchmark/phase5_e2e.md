# Phase 5 E2E 验收（2026-10-19）

## 环境

| 项 | 值 |
|---|---|
| 主机 | VM `ubuntu` |
| OS | Ubuntu 16.04 |
| 编译器 | g++ 5.4.0 |
| CMake | 3.5.1 |
| MySQL | 5.7.33（库 `cloud_media_test`，账号 `cm_test`） |
| Redis | 本机 6379，db 0 |
| 服务端 | `build/bin/cloud-media-server -c build/e2e.conf`，127.0.0.1:8004 |
| 数据目录 | `/home/colin/cm_e2e_data`（blobs/tmp/downloads 由服务端自建） |
| 客户端 | `build/bin/upload_cli` / `build/bin/download_cli` |
| 测试账号 | `13800000001`，`u_pwd_algo=1`（md5(salt+pwd)），密码仅来自 `CM_CLI_PASSWORD` |

测试文件：50 MiB 随机数据。本轮最终不变量是「本地源文件 / 服务端 blob / 下载结果」三者 MD5 一致。

## 编排脚本

`scripts/e2e_phase5.sh`：`set -euo pipefail`，从环境读取 `CM_CLI_PASSWORD`、`CM_MYSQL_PASSWORD`（可选 `MD5_REF`），自行停/启服务端并依次执行 A/B/C，逐步断言。

复跑：

~~~bash
. ~/.cm_e2e_env                 # CM_CLI_PASSWORD
export CM_MYSQL_PASSWORD=...    # 只放环境，不写脚本
cd ~/cloud-media && bash scripts/e2e_phase5.sh
~~~

本轮脚本输出：`ALL PASSED`，退出码 0，整轮 **8s**（A 3s / B 4s / C 1s）。

## A. 上传断点续传（杀客户端）

| 步骤 | 结果 |
|---|---|
| 源文件 MD5 | `f37f33a350626dd59fa1d1816403fc0d` |
| `--stop-after-chunks 6` | `STATUS=INTERRUPTED`，`SENT_CHUNKS=6`，退出 9 |
| DB `t_transfer_task` | `tr_status=1`，`tr_done_chunks=6`（总 13） |
| `-r` 续传 | `STATUS=OK`，`FILE_MD5=f37f…fc0d`，`UF_ID=16071` |
| 服务端 blob | `f_id=5168`，`f_md5=f37f…fc0d`，`f_size=52428800`，`f_state=1`，`f_ref_count=1` |

结论：只补发了缺失的 7 片，最终 MD5 与源文件一致。

## B. 上传中杀服务端 → 重启 → 续传

| 步骤 | 结果 |
|---|---|
| 源文件 MD5 | `2a23ab6b0a02c54c279a5a5edf1622d4` |
| 第 6 片后 `pkill -9` 服务端 | DB `tr_status=1`，`tr_done_chunks=6` |
| 注入 `tr_status=7` 后重启 | DB `tr_status=2`（U-6 `recoverOnStartup` 的 7→2 分支） |
| `-r` 续传 | `STATUS=OK`，`FILE_MD5=2a23…22d4`，`UF_ID=16070` |
| 服务端 blob | `f_id=5169`，`f_md5=2a23…22d4`，`f_size=52428800`，`f_state=1`，`f_ref_count=1` |

说明：上传中途杀进程只会留下 `tr_status=1`，不会进入 7；状态 7 只在合并阶段出现。为了真正覆盖 U-6 的 `7→2`，本轮在重启前把该任务注入为 7，重启后 DB 读回 2，再续传成功。

## C. 下载断点续传（杀客户端）

| 步骤 | 结果 |
|---|---|
| `--stop-after-blocks 6`（-j 4） | `STATUS=INTERRUPTED`，`BLOCKS_THIS_RUN=6`，退出 9 |
| 半成品 | `e2e_dl_*.bin.part` + `.part.state`（size/md5/block_size/bitmap） |
| `-r` 续传 | `STATUS=OK`，`BLOCKS_TOTAL=13`，`BLOCKS_THIS_RUN=7` |
| 下载结果 MD5 | `f37f33a350626dd59fa1d1816403fc0d`（与源文件、服务端 blob 一致） |

## 证据

- DB：A/B 任务完成后 `tr_status=4`、`tr_done_chunks=13`；中断时为 `status=1/done=6`。
- blob：两条链路均为 `f_state=1`、`f_ref_count=1`、`f_size=52428800`。
- `md5sum`：源文件、`e2e_dl_*.bin`、DB `f_md5` 三者相同。
- `server.log`（收口复跑，B 重启后）：

```
cloud-media-server listening on 0.0.0.0:8004 (loop=1, workers=4, data=/home/colin/cm_e2e_data)
2026-10-19 06:33:16.905 | INFO  | tid=1 | server_app.cpp:125 | trace=- | recoverOnStartup reset 1 merging tasks (7->2)
```

## 已知限制

1. `Logger` 已在 `runServer()` 入口初始化（console=true），`recoverOnStartup` 与 `cleanStaleTmp` 会写 INFO；E2E 脚本的 nohup 重定向可直接 grep 到 `7->2`。
2. 上传中途杀进程不会产生 7（见 B 的说明），因此 B 用注入 7 覆盖恢复分支。
3. 计划中的参考哈希 `25e317773f308e446cc84c503a6d1f85` 在本机 `~/cm_testdata/test_50m.bin` 上可直接复现；首轮手工 A 用的就是它且一致。编排脚本默认生成本轮随机 50 MiB，并断言三方一致。
4. `upload_cli -c` 会被服务端忽略（固定 4 MiB）；`download_cli --md5` 无论是否传都会做整文件 MD5 校验。
5. 服务端仍无请求级日志；7→2 已由启动 INFO 行覆盖，其余链路状态仍以 DB 查询为准。
6. E2E 使用 Redis db 0；脚本会停掉 8004 上已有的 `cloud-media-server` 再启动新实例。
