# Phase 7 第三批：修复轨 + 性能轨（P7-4）

> 测试日期：2026-10-20
> 结论速览：upload 10MB c=100 从 **35.3 → 86.7 MB/s（2.46×）**，P99 从 11.59s → **6.65s**；限速 R1 接线生效；ctest 43/43（含 DB 集成）。

## 0. 清态协议与干净基线

### 0.1 协议

每轮 before/after 前固定执行 `scripts/perf/clean_state.sh`：

~~~bash
bash scripts/bench_env.sh wipe          # 清库 + 清 blob
chown -R colin:colin ~/cm_bench_data    # wipe 以 root 跑，必须归还属主
sync && echo 3 > /proc/sys/vm/drop_caches
df -h /                                  # 记录余量：18G 可用
~~~

### 0.2 两个"劣化根因"，都不是磁盘

按要求先查 84MB/s 是否回得去，结论是**两个独立的测量/环境问题**：

| # | 现象 | 根因 | 处理 |
|---|---|---|---|
| 1 | 复测只有 47~56MB/s，怎么清态都上不去 | **VM 上 compiz 占了 0.67 核**（457 小时累计 CPU，桌面合成器在 2 vCPU 上和压测抢 CPU） | `service lightdm stop` + kill compiz，list 立刻从 712 → 1079 QPS |
| 2 | `thr=84.66MB/s` 与 `file_qps=4.2` 对不上（4.2×10MB=42MB/s） | **bencher 计数器 bug**：`g_bytes` 在 onFrame 里无条件累加，而 `completed` 只统计"测量窗口内开始"的请求，warmup 期间开始、窗口内完成的请求把字节数抬高了 ~2× | 改成 `countBytes/countChunk` 与 `wasMeasured` 同判定 |

**所以 P7-2 报告里的 upload 84.7MB/s 是虚高，真实值一直是 ~42MB/s**；写放大 2.0× 的结论不变（wchar 97.8 / 逻辑 50 ≈ 2.0，设备写/wchar=1.03）。P7-2/P7-3 的吞吐数字需按此修正，热点占比与占比结论不受影响。

### 0.3 干净基线（本轮 before 锚点）

upload 10MB，c=100，`--inflight 64`，warmup 10s × 3 次，清态：

| run | file_qps | MB/s | P50 | P99 | 失败 |
|---|---:|---:|---:|---:|---:|
| 0 | 3.8 | 38.3 | 12.72s | 14.78s | 0 |
| 1 | 3.5 | 35.3 | 10.65s | 11.59s | 0 |
| 2 | 3.4 | 34.0 | 10.76s | 11.57s | 0 |
| **中位** | **3.5** | **35.3** | **10.65s** | **11.59s** | 0 |

> 注意：`duration 15` 的短窗口在这台机器上不可用——单文件 P50 已 >10s，测量窗口比请求延迟还短，counted 的完成数会严重偏低。**本批统一 duration 30 / warmup 10**。

## 1. 修复轨

### R1 下载限速接线（D-5 / D-6）

改动：`server/logic/download_handlers.{h,cpp}` + `server/app/main.cpp`。

- **连接级令牌桶 2MB/s（容量 4MB）+ 用户级 8MB/s（容量 16MB）**，按 4MB 块计费。
- **流数上限**：`StreamLimiter` 全局 512 / 每用户 8，`DownloadOpen` 时抢槽位，超限回 **3006**。
- 令牌不足但等待 ≤ 2s → **不拒绝**，用 `conn.runAfter(waitMs)` 挂到 loop 定时器延迟重发（worker 线程不 sleep）；> 2s 才回 3006。
- 槽位归还：`DownloadDone` 或连接关闭（`server.setCloseCallback` → `onConnClose`）双保险。

| 场景 | 限速前（P7-2 裸能力） | 限速后（P7-5 修 bug 后） |
|---|---:|---:|
| download 10MB c=10 inflight=8 | 522 MB/s（修正值） | **28.2 MB/s**（10MB 未进稳态，见下） |
| **40MB 单流稳态** | 522 MB/s | **2.0 MB/s**（40MB / 20.0s，P50 20009ms） |
| **同用户 4×40MB** | 522 MB/s | **7.6 MB/s 聚合**（每流 1.9MB/s，P50 20951ms），≤ 8MB/s 用户上限 |
| 超限行为 | 无 | inflight=12 → `err=3006` 立即返回 |

> **28.2MB/s 不是稳态**：10MB 文件在 4MB 桶突发窗口内就跑完了（首块免等），所以看起来比 2MB/s 快，但那是突发不是持续速率。用 40MB（10 块）才能看到真实稳态：**2.0MB/s**，与配置完全一致。
>
> **P7-4 初版还有个 2× bug**：收到 Timeout 后用 runAfter 直接发块，**没有重新走 consume**，等于隔块漏计费 → 实测 3.9MB/s（2×）。P7-5 改为延迟后**重入 handleRange**（重新计费），单流立刻回到 2.0MB/s、4 流聚合 7.6MB/s。这条说明"限速必须闭环验证，只看 10MB 短文件会被桶容量骗过去"。

> 另外发现：bencher 收到 3006 后不 backoff，会打出 17 万条/12s 的失败日志；这是压测客户端的问题（未改），真实客户端应退避。

### R2 list_cli --limit

`limit(0) → limit(50)`，并在 encode 前把显式 `--limit 0` 归一到 50，对齐用法文案"0=50"。验证：不带 `--limit` 直接成功。

### R3 logger `[log] async` 键

`ServerConfig::logAsync`（默认 true）+ `loadServerConfig` 解析 `[log] async`（false/0 关闭）+ `main` 用 `cfg.logAsync` 替代硬编码 true。非法值按 true 处理，避免压测配置把日志关死。

## 2. 性能轨

### 2.1 O7-c：block CRC sidecar 懒化

改动：删掉 `upload_service.cpp` 里 completeUpload 的 `writeBlockCrc` 急调用（原来每次上传完成都把刚合并的 10MB 再读一遍算 CRC）。下载侧 `ensureBlockCrc` 早已存在并接在 `sendfile_block_sender.cpp:186`，按块懒算。

**省掉**：16.11% CPU + 1 趟 10MB 读。

### 2.2 O7-a：分片 MD5 不再收片时重算

先读代码：`upload_chunk_handler` 里对每个 4MB 分片算一次 MD5 只为比对客户端声明值；而 `mergeChunks` 在合并时**已经逐片重算并比对**，不一致进 `badIndexes` 让客户端重传。传输完整性另有帧 CRC（`Codec::verifyFrameCrc`）覆盖。所以这是**第三趟冗余**。

改动：删掉收片时的 `md5Hex(rq.data) != rq.chunkMd5` 检查（保留长度校验与 DAO 的 dup-md5 校验），契约变为"收片不校验内容，complete 兜底"。
测试同步改写：`UploadChunkBadCrcCloses` → `UploadChunkTamperedPayloadDeferredToMerge`（断言坏片被收下，坏片检测由 test_upload_complete 的 badIndexes 用例覆盖）。

### 2.3 O7-c + O7-a 合并效果（同一构建，无法拆分归因）

| 指标 | before（清态） | after（清态） | 变化 |
|---|---:|---:|---:|
| file_qps | 3.5 | **5.0** | +43% |
| 吞吐 | 35.3 MB/s | **50.3 MB/s** | +43% |
| P50 | 10.65s | 8.41s | −21% |
| P99 | 11.59s | **9.07s** | −22% |
| 失败率 | 0 | 0 | — |

> 两项都是"删一趟全量扫描"，单独拆分的成本是两次重建+两轮复测；按 perf 占比估算 O7-c 贡献约 16/28，O7-a 约 12/28。

### O7-b：merge 用 crc32_combine —— **跳过，前提不成立**

读了代码：merge 阶段只算整文件 **MD5**，**根本没有整文件 CRC 重扫**；profile 里那 28% 的 CRC 是「帧 CRC（12.84%）+ writeBlockCrc 回读（16.11%）」，后者已被 O7-c 删除。没有可合并的对象，`crc32_combine` 无处可用，**跳过**（若将来要改 sidecar 生成方式，正确做法是用上传期已算好的分片 CRC 直接填 sidecar，而不是 combine）。

### 2.4 O1：merge 直接写 blobs 路径

改动（`upload_service.cpp`）：合并输出从 `data/tmp/<taskId>/merged.bin.tmp` 改成 `data/blobs/aa/bb/.<taskId>.merged.tmp`（aa/bb 由**声明的** fileMd5 推导），发布阶段只剩一次**同目录 rename**；整文件 MD5 不符时就地 unlink，避免隐藏残留。

| 指标 | before（O7 后） | after（清态） | 变化 |
|---|---:|---:|---:|
| file_qps | 5.0 | **8.7** | +74% |
| 吞吐 | 50.3 MB/s | **86.7 MB/s** | +72% |
| P50 | 8.41s | 6.08s | −28% |
| P99 | 9.07s | **6.65s** | −27% |
| 设备写放大 | 2.0× | 1.0×（预期） | −50% |

残留检查：`find blobs -name "*.merged.tmp" | wc -l` = **0**；`tmp/` 下 133 个目录是 3 轮结束时仍在途、尚未 complete 的任务（到期由 ExpiryJob 清理），符合预期。

### 2.5 O3：backlog 调优 —— 做了，**无收益，已回退**

| 指标 | before（默认 128/256） | after（4096/8192） |
|---|---:|---:|
| 建连速率（干净 TIME_WAIT） | 1920 conns/s（P7-2） | **1942 conns/s** |
| P50 | 0.94ms | 0.35ms |
| P90 | 1043ms | 1016ms |
| P99 | 1043ms | **3039ms** |

8000 连接全部成功、CPU 只有 3%，**P90 的 1 秒不是 backlog，而是客户端 TIME_WAIT/端口回收与 accept 循环**。调大 backlog 只让 P99 更差（排队更久才被 accept），因此已 `sysctl -w` **回退到 128/256**，EPOLLEXCLUSIVE 也未做（P7-3 已判：只对 accept 惊群有意义，本场景无收益）。

> 测量注意：连续跑 8000 连接风暴会耗尽客户端临时端口（32768-60999），第 2、3 次会出现 ~10% connect 失败或 864 conns/s 的假劣化；必须等 TIME_WAIT 清空（~60s）再测。

### O2 / O4：**跳过**

- **O2 对象池**：O7-c/O7-a/O1 之后拷贝类热点的绝对量已下降，且当前瓶颈仍是 MD5（分摊后 <25%）；对象池需要改 Buffer/Frame 所有权，收益 ~5%，性价比低于继续砍哈希，留到 P7-5。
- **O4 线程池无锁化**：只对 list 有意义（内核锁 10.9% + eventfd 1.5% + task_switch 2.8%）。本批未动 list 路径，缺乏 after 对照，按"缺 before/after 视为未完成"的口径不虚报，留到 P7-5。

## 3. 累计收益

| 阶段 | file_qps | 吞吐 | P99 |
|---|---:|---:|---:|
| 干净基线 | 3.5 | 35.3 MB/s | 11.59s |
| +O7-c +O7-a | 5.0 | 50.3 MB/s | 9.07s |
| +O1 | **8.7** | **86.7 MB/s** | **6.65s** |
| 累计 | **2.49×** | **2.46×** | **−43%** |

与 P7-3 的预测对照：当时估计"O7 先做、O1 后做、合计 1.5× 空间"，实测叠加后是 2.46×——因为 O1 同时把设备写砍半，磁盘不再是并发上限，P50 从 Little's Law 的 10.6s 直接降到 6.1s。

## 4. 回归与红线

- ctest：**43/43 通过**（build-bench 未开 `ENABLE_DB_INTEGRATION_TESTS` 只注册 41 项；全量口径含 2 个 DB 集成项为 43，已用 build-cov 复跑全绿）。
- `scripts/check_cpp11.sh`：CLEAN（无 make_unique/optional/string_view/filesystem/泛型 lambda）。
- 红线 grep：`common/` 无 Qt、无 libav/SDL；`client/` 无 `#include "server/`。

## 5. 改动清单

~~~text
 benchmarks/bencher.cpp                   |  23 +++++-   # 计数器修正（bytes 与 completed 同判定）
 common/config/server_config.cpp          |   6 ++    # [log] async
 common/include/cm/config/server_config.h |   1 +
 server/app/main.cpp                      |  58 +++++---  # 限速接线：connKey/runAfter/onConnClose
 server/examples/list_cli.cpp             |   5 +-    # --limit 默认 50
 server/logic/download_handlers.cpp       | 124 ++++++++  # DownloadRateLimit
 server/logic/download_handlers.h         |  53 +++
 server/logic/upload_chunk_handler.cpp    |   9 +-    # O7-a
 server/logic/upload_service.cpp          |  18 ++-    # O7-c + O1
 server/tests/logic/test_upload_chunk.cpp |  15 +-    # 契约变更
 10 files changed, 264 insertions(+), 48 deletions(-)
~~~

## 6. 复现

~~~bash
bash scripts/perf/clean_state.sh
bash scripts/bench_env.sh start
source ~/cm_bench_env.sh
build-bench/bin/bencher -p 8014 -op upload -c 100 -t 4 -size 10 \
    -duration 30 -warmup 10 -runs 3 --inflight 64
build-bench/bin/bencher -p 8014 -op download -c 10 -t 2 -size 10 \
    -duration 20 -warmup 5 -runs 1 --setup-files 4 --inflight 8
~~~
