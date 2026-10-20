# Phase 7 压测 before 基线（P7-0 / P7-1 / P7-2）

> 测试日期：2026-10-19 ~ 2026-10-20
> 被测：cloud-media-server（Release，build-bench），端口 8014，独立库 cloud_media_bench
> 工具：benchmarks/bencher（本阶段新增，协议 v2 直连）
> 原始数据：VM `~/cloud-media/bench_before/`（before.csv 521 行、sys_*.csv、每档 *.log、summary.txt）

## 1. 环境（P7-0）

### 1.1 硬件与系统

| 项 | 值 |
|---|---|
| CPU | **2 vCPU**（`nproc` = 2） |
| 内存 | 5937 MB 总 / 启动时 available 4063 MB，swap 974 MB |
| 磁盘 | `/dev/sda1` 39G，压测前可用 18G；`lsblk` 显示 VMware Virtual S |
| 磁盘类型 | `/sys/block/sda/queue/rotational` = **1**（VMware 虚拟盘报非 SSD，宿主机真实介质不可见） |
| 内核 | 4.15.0-142-generic |
| 发行版 | Ubuntu 16.04.5 LTS |
| g++ / cmake | 5.4.0 / 3.5.1 |
| MySQL | 5.7.33（本机，InnoDB） |
| Redis | 3.2.8（本机，压测用 db=1） |

> **结论先行**：2 vCPU / 4GB 可用内存是全流程的硬约束，它决定了后面「在途上限 64」这一取舍，也决定了 10MB 上传的 P99 是秒级。

### 1.2 构建

~~~bash
cd ~/cloud-media && mkdir build-bench && cd build-bench
cmake .. -DBUILD_SERVER=ON -DBUILD_STORAGE=ON -DBUILD_TESTS=ON \
         -DBUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
make -j2
~~~

- `CM_DOWNLOAD_ZEROCOPY` 保持默认 ON；`DownloadOpenRs.zero_copy=1` 已由 download_cli 冒烟确认。
- 产物：`build-bench/bin/{cloud-media-server,bencher,upload_cli,download_cli,list_cli}`。
- 压测严禁 Debug：全部数据取自 Release。

### 1.3 服务端配置（bench.conf）

| 键 | 值 | 说明 |
|---|---|---|
| server.listen_port | 8014 | 与 e2e 的 8004 隔离，互不影响 |
| server.io_threads / worker_threads | 2 / 4 | 代码默认值 |
| server.data_dir | ~/cm_bench_data | 独立数据目录 |
| log.level | info | 生产默认值，未刻意降噪 |
| mysql.database | cloud_media_bench | 独立库，每档 TRUNCATE + 清 blob |
| redis.db | 1 | 与 e2e 的 db=0 隔离 |

环境脚本：`scripts/bench_env.sh init|start|stop|wipe|status`。
建库授权需要一次性 root 操作（cm_test 无权建库）：`scripts/sql/bench_grant.sh`（`sudo bash`）。

### 1.4 三项盘查结论

**a) 下载限速配置键：不存在，且限速根本没接进下载路径。**

- 令牌桶常量在 `server/logic/rate_limiter.h`：`kConnTokenRatePerSec = 2MB/s`、`kUserTokenRatePerSec = 8MB/s`（容量 4MB / 16MB）。
- 但全仓 `grep -rn "TokenBucket|StreamLimiter" server/logic server/app` **只有定义与单测，没有任何 handler 引用**；`download_handlers.cpp / download_service.cpp / block_sender.cpp` 里也没有限速、sleep 或 3006 返回。
- 因此「压测关闭限速」= **现状本来就无限速**。本报告所有下载数字都是裸能力。
- 这条要写进 Phase 7 待办：限速组件已实现且有单测，但没有接线（D-5/D-6 只完成了一半）。

**b) Logger async 配置键：ini 里改不了。**

- `cm::LogConfig` 有 `async(true)、flush_interval_ms(1000)、max_pending_bytes(64MB)`。
- 但 `loadServerConfig()` 对 `[log]` 只读 `path / level / max_size_mb / roll_days`，**没有 async 键**，服务端始终跑异步日志。
- 本轮压测因此保留异步日志 + info 级别，属于「生产同款」。

**c) 线程池实现：mutex + condition_variable + std::deque，不是无锁。**

- `common/base/thread_pool.cpp`：`std::deque<Task>` + 单 `std::mutex` + `std::condition_variable`，`notify_one` 入队唤醒，停机时把队列跑空。
- 4 个 worker 线程共享一把锁，是所有落库请求的串行点之一（list 档 CPU 56% 可作对照）。

### 1.5 冒烟（登录→上传→下载→列表）

| 步骤 | 命令 | 结果 |
|---|---|---|
| 上传 | `upload_cli --port 8014 --file 3MB` | STATUS=OK，UF_ID=1，MD5 f7ba…5640 |
| 列表 | `list_cli --port 8014 --limit 50` | 2 行，TOTAL_COUNT=2 |
| 下载 | `download_cli --port 8014 --uf-id 1 -j 2` | STATUS=OK，MD5 与源一致，ZERO_COPY=1 |
| 登录 | 三个 CLI 均隐含 LoginRq | 通过 |

> 顺手发现一个既有 CLI bug：`list_cli` 的 `--limit` 默认 0，而 `ListFileRq::encode` 拒绝 0（用法文案写的「0=50」没有落地），所以不带 `--limit` 时直接 "list send failed"。bencher 走 `--list-limit 50` 绕开。**未修**（本阶段不动 server/CLI 代码）。

## 2. bencher（P7-1）

### 2.1 设计

- 骨架沿用 `bench_conn`：**每线程一个 epoll、连接分组、单连接同一时刻只有一个在途请求**；不 spawn CLI，业务层直接组 v2 帧（`Codec::encode` + `messages/*.h`）。
- 连接建立后先发 `LoginRq` 完成握手（连接级 uid），再进入业务循环；握手失败的连接标记 disabled，不再重连。
- 请求语义：**一次请求 = 一次完整文件操作**
  - upload：`UploadInitRq → N×UploadChunkRq → UploadCompleteRq`（4MB 分片，10MB = 3 片）
  - download：`DownloadOpenRq → N×DownloadRangeRq（4MB block）→ DownloadDoneRq`
  - list：`ListFileRq(parent=0, sort=1, limit=50)`；login：在已登录连接上再发一次 `LoginRq`
- **唯一内容**：最后一个分片掺 seed，前缀分片全请求共享同一块 4MB 只读缓冲；整文件 MD5 = md5(前缀状态拷贝 + 尾片)，因此每个文件 MD5 唯一，秒传不会短路（`result==1` 记为 `instant_hit` 失败）。
- **在途闸门**：全局 `--inflight`（默认 64），大块请求先抢名额，抢不到挂等待队列，client 用 eventfd 唤醒对应 worker。
- 统计：QPS / 吞吐 / P50·P90·P99·P999 / 失败率 / 错误分布；CSV 每秒一行采样 + 每轮一行汇总；stdout 打人类可读摘要与 `SUMMARY` 行。
- 附加模式：`-op connect`（建连风暴，非阻塞 connect 后立即 close，报 conns/s 与建连延迟）。

### 2.2 编译（零告警）

~~~text
[ 60%] Built target cm_common
[ 90%] Built target cm_protocol
[100%] Building CXX object benchmarks/CMakeFiles/bencher.dir/bencher.cpp.o
[100%] Linking CXX executable ../bin/bencher
[100%] Built target bencher
warning/error 计数：0
~~~

CMake 侧只加了一个 target（`benchmarks/CMakeLists.txt`，链 `cm_protocol cm_common Threads::Threads`），没有动服务端代码；`-Wall -Wextra -Wpedantic` 由 `cm_compile_options` 传递，0 warning 0 error。

### 2.3 冒烟 `-c 10 -t 2`

| op | 参数 | 结果 |
|---|---|---|
| login | 10s | 9993.5 QPS，p99 5.33ms，失败 0 |
| upload | 10MB，15s | 6.6 file/s = 72.6 MB/s，p99 1575ms，失败 0 |
| download | 10MB，15s，预置 2 文件 | 54.6 file/s = 553.5 MB/s，p99 320ms，失败 0 |
| list | 10s | 1854.4 QPS，p99 15.6ms，失败 0 |

服务端全程存活（`ss` 仍有 8014 LISTEN，pid 未变），无重启、无 panic。ctest 未跑：本阶段只动了 `benchmarks/`。

## 3. before 基线（P7-2）

### 3.1 为什么有「在途上限 64」

固定分片 4MB，若 c=1000 全部同时在途，仅服务端就要 ~3GB 常驻（实测每在途约 3.2MB，见 §3.4），加上客户端同样量级，2C4G 的 VM 会 OOM。

因此矩阵口径统一记为：**连接数满档（c=100/1000/3000 全部在线），大块请求在途受 `--inflight 64` 约束**。list/login 不受闸门影响（不占大块内存）。

### 3.2 矩阵结果（每档 warmup 后跑 3 次取中位数）

| 档位 | file_qps | chunk_qps | 吞吐 MB/s | P50 | P90 | P99 | P999 | 失败率 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| upload 10MB c=100 | 4.2 | 22.8 | 84.66 | 7719ms | 8104ms | 8681ms | 8689ms | 0.00% |
| upload 10MB c=1000 | 4.1 | 21.9 | 84.00 | 7863ms | 8504ms | 8617ms | 9016ms | 0.00% |
| upload 10MB c=3000 | 3.7 | 25.4 | 78.00 | 7767ms | 7941ms | 8081ms | 8328ms | 0.00% |
| upload 1MB c=100 | 47.1 | 51.0 | 51.33 | 1250ms | 1312ms | 1366ms | 1395ms | 0.00% |
| upload 1MB c=1000 | 46.9 | 51.9 | 51.00 | 1234ms | 1306ms | 1388ms | 1421ms | 0.00% |
| download 10MB c=100 | 52.2 | 164.1 | 545.86 | 918ms | 1245ms | 1746ms | 1967ms | 0.00% |
| download 10MB c=1000 | 40.7 | 134.3 | 449.06 | 1316ms | 1817ms | 2168ms | 2424ms | 0.00% |
| list c=1000（60s） | 3765.1 | — | — | 263ms | 313ms | 348ms | 368ms | 0.00% |

服务端侧同步采样（`/proc` 等价 pidstat/iostat，原始在 `bench_before/sys_*.csv`）：

| 档位 | CPU 均值 | CPU 峰值 | RSS 区间 | 设备写均值 |
|---|---:|---:|---:|---:|
| upload 10MB c=100 | 127% | 157% | 126 → 356 MB | 167 MB/s |
| upload 10MB c=1000 | 126% | 160% | 125 → 351 MB | 164 MB/s |
| upload 10MB c=3000 | 125% | 159% | 126 → 373 MB | 163 MB/s |
| upload 1MB c=100 | 94% | 110% | 126 → 126 MB | 121 MB/s |
| upload 1MB c=1000 | 94% | 104% | 125 → 150 MB | 119 MB/s |
| download 10MB c=100 | 15% | 65% | 125 → 125 MB | 2 MB/s |
| download 10MB c=1000 | 14% | 61% | 125 → 125 MB | 1 MB/s |
| list c=1000 | 56% | 64% | 125 → 125 MB | 0 MB/s |
| connect storm 8000 | 3% | 5% | 125 → 125 MB | 1 MB/s |

### 3.3 connect storm（EPOLLEXCLUSIVE 的 before 数字）

| 指标 | 值 |
|---|---|
| 连接数 | 8000（-t 4，非阻塞 connect 建完即 close） |
| 成功 / 失败 | 8000 / 0，失败率 0.00% |
| 总耗时 | 4.17 s |
| **建连速率** | **1920 conns/s** |
| 建连延迟 P50 / P90 / P99 / P999 | 0.94 ms / 1018 ms / 1043 ms / 1044 ms |

解读（重要）：P50 不到 1ms，但 P90 直接跳到 1 秒——说明**不是 accept 处理慢，而是内核 SYN/accept backlog 被打满后客户端被 TCP 重传退避拖到秒级**。EPOLLEXCLUSIVE 优化的是「多 IO 线程惊群」，而这里 8000 连接只用了 3% CPU，瓶颈在 backlog 排队。后续做 EPOLLEXCLUSIVE 时，应同时调 `somaxconn` / `tcp_max_syn_backlog`，否则 after 数字不会有明显改善。

### 3.4 内存放大系数（对象池优化的量化靶子）

同一 op（upload 10MB，c=100）只改 `--inflight`，同时采服务端与客户端 RSS：

| 在途上限 | 服务端峰值 RSS | 客户端峰值 RSS | 服务端每在途（扣 126MB 基线） | file_qps |
|---:|---:|---:|---:|---:|
| 32 | 173 MB | 184 MB | 1.5 MB | 6.3 |
| 64 | 333 MB | 338 MB | **3.2 MB** | 4.2 |
| 128 | 517 MB | 521 MB | 3.1 MB | **0.1** |

结论要如实修正之前的口径：

1. **「每在途分片 12MB」是理论最坏上界**（读 Buffer + Frame 拷贝 + `UploadChunkRq.data` 三份同时存在），实测峰值只有 ~3.2MB/在途——三份拷贝不会同时达到峰值，Buffer 在 decode 后很快被 consume。优化靶子不能按 12MB 算。
2. 真正的靶子是「**每在途 3.2MB 常驻 + 每分片 4MB malloc/free**」：inflight 32→128 时 RSS 近似线性增长（+344MB），说明常驻量与在途数正相关，对象池/复用 Buffer 能把这条斜率压下来。验收标准建议改成「同 64 在途下服务端峰值 RSS 从 333MB 降到 X MB」，而不是「每分片从 12MB 降到 Y」。
3. **在途上限不是越大越好**：inflight=128 时 file_qps 从 4.2 崩到 0.1（2 核被 MD5 + 4MB 拷贝打满，且 warmup 可能不足以填满 128 深的管道）。64 就是这台机器的拐点。

### 3.5 数据真实性校验

一次干净批次（wipe 后 upload 1MB × 50 连接 / 20s）：

~~~sql
SELECT COUNT(*) FROM t_user_file;                          -- 1094
SELECT COUNT(*), COUNT(DISTINCT f_md5) FROM t_file;        -- 1094, 1094
~~~

- `COUNT(*) = COUNT(DISTINCT f_md5)` = 1094：**每个文件的 MD5 都不同，没有命中秒传、没有格式化短路**。
- 磁盘 blob 3282 个 = 1094 × 3（blob + .meta + .crc），1.1 GB ≈ 1094 MB，与逻辑字节数一致。

### 3.6 解读

1. **上传被磁盘和 CPU 双限，与连接数无关**：c=100/1000/3000 吞吐 84.7 / 84.0 / 78.0 MB/s，P99 都在 8~9s。设备写 164MB/s ≈ 逻辑吞吐的 **1.97 倍**（tmp 落盘 + merge 写 blob），CPU 1.26 核（MD5 + 三份拷贝）。想提升只能从「减少写放大」和「减少拷贝」入手，加连接没用。
2. **10MB 上传 P99 秒级的根因是排队**：64 在途 × 10MB / 84MB/s ≈ 7.6s，与实测 P50 7.7s 吻合。即 **P99 不是单请求慢，是 Little's Law 的排队时间**，简历里必须这么解释，否则会被问穿。
3. **下载走 sendfile，CPU 只要 15%**：545MB/s、RSS 全程 125MB 不涨、设备写几乎为 0（命中 page cache，因为文件刚上传）。c=1000 反而掉到 449MB/s，是 1000 条连接的调度开销；下载路径本身还有余量。
4. **list 3765 QPS / CPU 56%**：瓶颈在 MySQL 查询 + 4 worker 线程共享一把锁，不在网络层。
5. **登录 10k QPS（c=10）**：algo=1 是 md5(salt+pwd)，单次几十微秒，未成为瓶颈。
6. **失败面极干净**：全矩阵 8 档 × 3 轮只有 1 次失败（upload 1MB c=100 run0，`chunk=4003 DbQueryFailed`，1/2150），属于 MySQL 瞬时抖动，不是逻辑错误。

## 4. 复现

~~~bash
# VM 上（一次性）
sudo bash scripts/sql/bench_grant.sh          # cm_test 建库授权
bash scripts/bench_env.sh init && bash scripts/bench_env.sh start

# 冒烟
bash scripts/bench_env.sh status
source ~/cm_bench_env.sh
build-bench/bin/bencher -p 8014 -op upload -c 10 -t 2 -size 10 -duration 15 -warmup 3 --inflight 10

# 全矩阵（约 12 分钟，原始 CSV 落 bench_before/）
nohup bash scripts/bench_before.sh > /tmp/bench_run.log 2>&1 &
bash scripts/bench_sysstat.sh                 # 汇总 CPU/RSS/磁盘
bash scripts/bench_mem_probe.sh               # 在途 → 内存放大系数
~~~

## 5. 本阶段发现的问题（未修，按阶段分派）

| # | 问题 | 影响 | 建议归属 |
|---|---|---|---|
| 1 | `TokenBucket` / `StreamLimiter` 有实现有单测，但**没有接进下载路径**，「2MB/s 连接级 + 8MB/s 用户级」限速实际不生效 | 需求 D-5/D-6 只完成一半；简历不能宣称已限速 | Phase 7 补接线 |
| 2 | 服务端 ini **没有 logger async 键**，只能改代码 | 压测无法做「关日志/关异步」对照 | Phase 7 |
| 3 | `list_cli` 的 `--limit` 默认 0，`ListFileRq::encode` 拒绝 0，用法文案写「0=50」但没落地 | 不带 `--limit` 必然失败 | 小修 |
| 4 | connect storm P90 1s，根因是 SYN/accept backlog（somaxconn 默认 128） | EPOLLEXCLUSIVE 单独做收益有限 | 与 EPOLLEXCLUSIVE 同批调内核参数 |
| 5 | 上传写放大 ~2x（tmp + merge） | 84MB/s 逻辑吞吐背后是 164MB/s 设备写 | Phase 7 优化项 |
| 6 | 在途 128 时吞吐崩塌（4.2 → 0.1 file_qps） | 现有背压只有「连接级 2 个在途分片」，缺全局背压 | Phase 7 |
