# Phase 7 第二批：火焰图与热点→优化映射（P7-3）

> 测试日期：2026-10-20
> 目标：把 §3.6 的「磁盘和 CPU 双限」拆成可执行的热点清单，并确认 2x 写放大具体落在哪一段
> 产物：`docs/benchmark/flamegraph_upload_before.svg`、`docs/benchmark/flamegraph_list.svg`
> 原始数据：VM `~/perf_out/`（perf.data / folded / top.txt / wa_*.log / io_sample.csv）

## 1. perf 是否可用

| 项 | 结果 |
|---|---|
| 内核 | 4.15.0-142-generic |
| 安装 | `apt-get install -y linux-tools-common linux-tools-generic` 成功，但仓库里只有 **4.4.0-210** 的 tools（Ubuntu 16.04 没有 4.15 对应包） |
| `/usr/bin/perf` | **不可用**：包装器直接报 "perf not found for kernel 4.15.0-142" |
| 实际可用 | `/usr/lib/linux-tools/4.4.0-210-generic/perf`（perf 4.4.262），采样/报告/调用栈均正常 |
| 权限 | `kernel.perf_event_paranoid = 3` → **必须 root 采样** |
| 结论 | **perf 可用**，无需 fallback；代价是内核版本不匹配的 4.4 工具 + 全程 sudo |

调用栈有个坑：Release 默认 `-fomit-frame-pointer`，`perf record -g` 只能展开到叶子函数（137/237 行是 `[unknown]`）。因此另建了一个只用于出图的 `build-prof`（Release + `-fno-omit-frame-pointer`），采样后再核对热点占比与 Release 一致：

| 符号 | Release (build-bench) | build-prof |
|---|---:|---:|
| cm::Md5::transform | 40.83% | 41.07% |
| cm::Crc32::update | 28.24% | 28.55% |
| mpt_put_msg_frame | 10.81% | 10.99% |

差异 < 0.3 个百分点，**火焰图可代表 Release 行为**；表格里的占比取 Release 那一列。

FlameGraph 脚本：GitHub 被墙（wget rc=4），走 gitee 镜像 `gitee.com/mirrors/FlameGraph/repository/archive/master.tar.gz` 成功，解到 `~/FlameGraph`。

## 2. 采样场景

| 场景 | 参数 | 采样时吞吐 | 样本数 | SVG |
|---|---|---|---:|---|
| upload 10MB | `-c 100 -t 4 -size 10 --inflight 64`，record 30s | 4.3 file/s / 51.85 MB/s / p99 14.7s | 3730 | `flamegraph_upload_before.svg` |
| list | `-c 1000 -t 4`，60s，record 30s | 2311 QPS / p99 588ms | 1138 | `flamegraph_list.svg` |

> 采样期两个场景都低于 before 表（upload 84.7→51.9MB/s，list 3765→2311 QPS），原因是机器已累计约 20GB 上传（磁盘 63% 占用 + 脏页回写压力）。用同一个 Release 二进制复测 upload 得到 56.5MB/s，**证明这是环境劣化，与 build-prof / perf 无关**；热点占比不受影响（见 §1 对照表）。

## 3. upload 10MB Top20

| # | 热点函数 | 占比 | 归属 | 能被哪个优化消除 |
|---:|---|---:|---|---|
| 1 | cm::Md5::transform | 41.07% | **MD5** | O7-a（分片校验与 merge 整文件校验合并，省 1 趟） |
| 2 | cm::Crc32::update | 28.55% | **CRC** | O7-b（帧 CRC 12.84%）+ O7-c（block CRC sidecar 16.11%） |
| 3 | mpt_put_msg_frame [k] | 10.99% | 磁盘写放大 | O1（vmw_pvscsi 提交 SCSI 命令） |
| 4 | copy_user_enhanced_fast_string [k] | 3.14% | memcpy | O2（4MB 帧收发拷贝） |
| 5 | __memcpy_avx_unaligned | 1.88% | memcpy | O2（Frame / data 复制） |
| 6 | __softirqentry_text_start [k] | 1.64% | 其他（网络/块软中断） | 跳过，非应用可控 |
| 7 | __memset_avx2 | 0.99% | memcpy | O2（缓冲清零） |
| 8 | __lock_text_start [k] | 0.97% | 锁 | O4（上传场景占比低，收益可忽略） |
| 9 | __block_commit_write.isra.34 [k] | 0.91% | 磁盘写放大 | O1 |
| 10 | __memmove_avx_unaligned | 0.80% | memcpy | O2（Buffer compact） |
| 11 | iov_iter_fault_in_readable [k] | 0.67% | memcpy | O2（大块 send/recv 触页） |
| 12 | generic_file_read_iter [k] | 0.51% | 磁盘写放大 | O1（merge 回读 tmp） |
| 13 | ext4_finish_bio [k] | 0.32% | 磁盘写放大 | O1 |
| 14 | __add_to_page_cache_locked [k] | 0.24% | 其他（页缓存） | 跳过，内核行为 |
| 15 | cm::Md5::update | 0.19% | MD5 | O7-a |
| 16 | _raw_spin_lock [k] | 0.16% | 锁 | O4（同上，可忽略） |
| 17 | ext4_da_get_block_prep [k] | 0.16% | 磁盘写放大 | O1 |
| 18 | get_page_from_freelist [k] | 0.16% | 其他（分配） | O2 |
| 19 | __do_page_fault [k] | 0.13% | 其他（缺页） | 跳过 |
| 20 | clear_page_dirty_for_io [k] | 0.13% | 磁盘写放大 | O1 |

按类别汇总：**MD5 + CRC = 69.6%**，磁盘路径 ≈ 13.9%，拷贝 ≈ 6.8%，锁 ≈ 1.1%，其余内核杂项 ≈ 8%。

含子调用的 inclusive 占比（同一份 folded 数据算的，比自时间更能说明"钱花在哪个阶段"）：

| 阶段 | inclusive |
|---|---:|
| `ThreadPool::workerLoop`（所有业务线程） | 94.72% |
| `UploadService::completeUpload`（merge + sidecar + 落库） | 51.88% |
| ↳ `writeBlockCrc`（block CRC sidecar） | **16.11%** |
| ↳ 合并本身（读 tmp + 整文件 MD5 + 写 merged） | ≈ 35.8% |
| `UploadChunkHandler`（分片 MD5 校验 + 落盘 + 落库） | 12.44% |
| `verifyFrameCrc`（帧 CRC） | 12.84% |
| `MysqlConn` / `TransferDao` | 0.11% / 0.16% |

**这条是本次最有价值的数字**：`writeBlockCrc` 一个函数吃掉 **16.11%** 的 CPU，而它的产物只是一个 18 字节的 `.crc` sidecar；同一份数据在 `ensureBlockCrc` 里本来就能按需生成。

## 4. list 1000 连接 Top20

| # | 热点函数 | 占比 | 归属 | 能否被优化消除 |
|---:|---|---:|---|---|
| 1 | __lock_text_start [k] | 10.90% | 锁 | O4（socket/调度器内核锁，用户态锁优化只能吃掉一部分） |
| 2 | do_syscall_64 [k] | 3.34% | 其他（syscall 入口） | 跳过 |
| 3 | _int_malloc | 3.16% | memcpy/分配 | O2（每请求 Frame/字符串拷贝） |
| 4 | finish_task_switch [k] | 2.81% | 锁/调度 | O4（worker 与 reactor 频繁切换） |
| 5 | malloc | 1.58% | 分配 | O2 |
| 6 | eventfd_write [k] | 1.49% | 锁/唤醒 | O4（每个响应唤醒一次 reactor） |
| 7 | exit_to_usermode_loop [k] | 1.41% | 其他 | 跳过 |
| 8 | __memcmp_sse4_1 | 1.23% | 其他（map key/字符串比较） | 跳过 |
| 9 | malloc_consolidate | 1.23% | 分配 | O2 |
| 10 | __fget [k] | 1.14% | 其他（fd 查找） | 跳过 |
| 11 | _IO_vfscanf | 1.14% | 其他（格式化，待查） | 跳过 |
| 12 | __memcpy_avx_unaligned | 1.14% | memcpy | O2 |
| 13 | _int_free | 1.14% | 分配 | O2 |
| 14 | sys_epoll_ctl [k] | 1.05% | 其他（每次 MOD 事件） | 跳过（注册/注销可合并，但收益小） |
| 15 | __inet_lookup_established [k] | 0.97% | 网络 | 跳过 |
| 16 | tcp_sendmsg_locked [k] | 0.97% | 网络 | 跳过 |
| 17 | libpthread 0x1051d | 0.79% | 锁 | O4 |
| 18 | kmem_cache_alloc_node [k] | 0.70% | 分配 | O2 |
| 19 | operator new | 0.70% | 分配 | O2 |
| 20 | process_backlog [k] | 0.62% | 网络 | 跳过 |

inclusive：`ThreadPool` 32.34%、`FileService`（列表查询/组装）25.13%、`MysqlConn` **11.78%**、`ConnectionDecoder` 0.79%、`EpollPoller` 0.62%。

线程维度（`perf report -s pid`）：4 个 worker 各约 20%，reactor 主线程 15.38%，logger 线程 2.72%——**80% 的 CPU 在 worker 里**。

> 与 §3.6 的关系：那里的结论是「list 瓶颈在 MySQL + 一把锁，不在网络层」。火焰图**细化而非推翻**它：MySQL 客户端 11.78%、内核锁 10.9%、malloc 系 ≈ 9%、syscall/网络 ≈ 9%，没有单一压倒性热点。也就是说 list 这一档**没有"一刀砍掉 30%"的机会**，最现实的收益来自分配器（O2）与线程唤醒（O4），各约 10%。

## 5. 写放大四段占比（决定 O1 具体改哪里）

### 5.1 两种口径的测量

同一 upload 场景，10 秒稳态窗口同时采 /proc/&lt;pid&gt;/io 与 /proc/diskstats（VM 没装 sysstat，iostat 不可用，/proc/diskstats 是等价口径）：

| 指标 | 数值 | 说明 |
|---|---:|---|
| 逻辑吞吐 | 45.3 MB/s | 该窗口磁盘已劣化，绝对值偏低，比值仍成立 |
| rchar（用户态读） | 135.5 MB/s | = 3.0 × 逻辑 |
| wchar（用户态写） | 97.8 MB/s | = 2.16 × 逻辑 |
| **块层读** | **0.0 MB/s** | 本地两趟读全部命中 page cache |
| 块层写 / wchar | 100.8 / 97.8 = **1.03** | 块层没有额外放大 |

以及 before 的 164MB/s ÷ 84.7MB/s = **1.97×**，与本次 2.16× 同量级（差异来自窗口内是否正好有 merge 落盘）。

### 5.2 分段归属（strace -y 按 fd 路径聚合）

| 段 | 每 10MB 文件 | 实测（15s 窗口） | 结论 |
|---|---|---|---|
| (a) tmp 分片落盘 | 10MB **写** | 348MB / 94 次（约 3.7MB/次） | 占写侧 50% |
| (b) merge 重读 tmp | 10MB **读** | 411MB / 392 次（1MB/次） | 读侧，命中 page cache |
| (c) merge 写 `merged.bin.tmp` → rename 进 blobs | 10MB **写** | 166MB / 158 次（1MB/次） | 占写侧 50% |
| (d) **block CRC sidecar 回读 blob** | 10MB **读** | 299MB / 285 次（1MB/次） | 读侧，命中 page cache，**用户没列的第 5 段** |
| (e) CRC/MD5 重算 | 0 字节 IO | — | 纯 CPU，见 §3：占 69.6% |

### 5.3 结论：2x 到底花在哪

1. **写侧 2.0× 全部由 (a) + (c) 构成，各占 50%**。blob 的"落盘"其实是 rename，不产生写字节；所谓"tmp→blob 双 IO"精确说是"分片写一次 + 合并结果写一次"。
2. **读侧不是 2× 而是 3×**：网络收包 1×（rchar 里包含 socket read）+ merge 重读 1× + **block CRC 回读 1×**。后两趟块层读为 0，全是 page cache，所以它们吃的是 **CPU 和内存带宽，不是磁盘**。
3. **(d) 是意外的第 5 段，也是最该砍的**：`writeBlockCrc` 在每次上传完成时把刚合并好的文件**完整再读一遍**算每块 CRC，占 16.11% CPU + 多一趟 10MB 读。而下载路径已有 `ensureBlockCrc` 懒生成机制——上传时急算 purely 是多余。
4. (e) 的"重算"是 4 趟全量扫描/字节：帧 CRC、分片 MD5、merge 整文件 MD5、block CRC。这就是 MD5+CRC 占 69.6% 的根因。

## 6. 热点 → 优化映射结论

| 优化 | 热点是否支持 | 依据 | 建议 |
|---|---|---|---|
| **O1 上传写放大**（merge 直接写 blob，去掉 tmp→blob 二次写） | ✅ 支持，但**不是第一优先** | 磁盘路径合计 ≈13.9%（mpt_put 11.0% 为主）；设备写 2.0× → 改后 1.0× | 做，但排在 O7 之后：CPU 已到 1.26/2 核，单做 O1 吞吐提升有限（估计 +10~15%），与 O7 叠加才有 1.5× 空间 |
| **O2 每分片拷贝/对象池** | ⚠️ 次要 | upload: memcpy+memset+memmove+copy_user ≈ 6.8%；list: malloc 系 ≈ 9.0% | 做，收益 ~5%（upload）/ ~8%（list）；顺手把 Buffer 常驻 3.2MB/在途压下去（见 before §3.4） |
| **O3 backlog + EPOLLEXCLUSIVE** | ❌ **本场景跳过** | upload/list 采样里 EpollPoller 0.03%/0.62%，accept 侧无热点；8000 连接风暴 CPU 仅 3% | 只对 connect storm 有意义（P90 1s 是 backlog），**不要指望它提升上传/列表吞吐** |
| **O4 线程池无锁化** | ✅ 仅 list | list: 内核锁 10.9% + eventfd_write 1.5% + task_switch 2.8% ≈ 15%；upload 侧 <1.5% | 做 list 版收益 ~10%，但要先解决"每响应一次 eventfd 唤醒"（可批量唤醒） |
| **O5 MySQL 批量** | upload ❌ / list ✅(弱) | upload: MysqlConn 0.11%；list: 11.78% | upload 侧**跳过**（占比可忽略，批量还要改 ack 语义）；list 侧最多 ~10%，性价比低于 O2 |
| **O6 日志** | ❌ **跳过** | Top20 无任何 logger 符号；异步日志落盘在后台线程，调用方只 memcpy 入队 | 维持现状，不作为优化项 |
| **O7 哈希管线（本次新增）** | ✅✅ **第一优先** | Md5 41.07% + Crc32 28.55% = 69.6%；4 趟全量扫描/字节 | 见下 |

### O7 拆成三步（按性价比排序）

| 步骤 | 内容 | 省掉 | 预期 |
|---|---|---|---|
| O7-c | 上传时不再急算 block CRC，改为下载首次访问时由 `ensureBlockCrc` 生成（该懒路径**已经实现并接在下载侧**：`sendfile_block_sender.cpp:186`） | 16.11% CPU + 1 趟 10MB 读 | **单步 ~-16% CPU**，改动最小：删掉 `upload_service.cpp:716` 的 `writeBlockCrc` 急调用即可 |
| O7-b | 帧 CRC（`verifyFrameCrc`）改为可协商：长连接建立时协商"大帧走业务校验"，或抽样校验 | 12.84% CPU | ~-13% CPU |
| O7-a | 分片 MD5 与 merge 整文件 MD5 合并（分片校验降级为可选/抽样，或只校验首尾片） | ≈12% CPU | ~-12% CPU |

三步合起来 —— 理论上限 ≈ 1/(1-0.41) ≈ **1.7×**，但实际会受磁盘写 2.0× 限制；所以正确顺序是 **O7-c → O1 → O7-b/a**，先把 CPU 从"哈希"挪到"能干活的余量"，再把写放大砍半。

## 7. 采样噪声与偏差

- 99Hz × 30s ≈ 3000 样本，单个符号的 ±0.5% 不可信；表中只对 ≥0.5% 的项做归因。
- strace 期间 ptrace 让所有线程的系统调用都变慢，分段表里的 MB/s **只用于确认"存在哪几段、每段调用粒度"，绝对值不用**；比值取自不挂 strace 的 `/proc/io` 与 `/proc/diskstats` 测量。
- 采样期机器已劣化（吞吐 84.7 → 51.8），占比可信、绝对吞吐不可与 before 表直接比。
- `build-prof` 只加了 `-fno-omit-frame-pointer`，用于展开调用栈；热点占比对照见 §1。

## 8. 复现

~~~bash
# perf（4.15 内核没有官方 tools，用 4.4 的二进制 + root）
sudo apt-get install -y linux-tools-common linux-tools-generic
sudo /usr/lib/linux-tools/4.4.0-210-generic/perf --version

# 出图用 build-prof（Release + 帧指针）
cmake -S . -B build-prof -DBUILD_SERVER=ON -DBUILD_STORAGE=ON -DBUILD_TESTS=OFF \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-fno-omit-frame-pointer
cmake --build build-prof -j2 --target cloud-media-server
BENCH_BIN=$PWD/build-prof/bin bash scripts/bench_env.sh start

# 采样 + 火焰图（脚本在 VM 上：~/perf_run.sh）
sudo bash ~/perf_run.sh upload10_c100 -op upload -c 100 -t 4 -size 10 -duration 70 -warmup 10 --inflight 64
sudo bash ~/perf_run.sh list_c1000     -op list    -c 1000 -t 4 -duration 60 -warmup 10
~~~
