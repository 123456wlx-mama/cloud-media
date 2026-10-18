# Phase 5 实施计划（业务层：上传 / 下载 / 分享 / 回收站）

**Created:** 2026-10-18
**Status:** In Progress（P0-1~P0-4 代码已落盘；未执行构建/测试，H1 对拍门槛尚未验证）
**Source Spec:** docs/specs/06-业务设计.md（v0.1）
**Review Gate:** 06 文档第 1.3 节的 4 项冲突裁决（号段、分享码、sendfile+CRC、物理回收）需在开工前确认；本文按裁决已落地为任务。
**P0 Exception:** 用户已确认 P0-4 允许对 cache_store.cpp 做最小改动，仅限 CacheKey::share/shareMiss 与新增 uploadReq/shareReq，不改 CacheStore 逻辑。
**Execution Policy:** 本文件中的 cmake / make / ctest / mysql / 压测命令均只作为验收文本，由人工执行；本会话不运行任何构建、测试、数据库或网络命令。

## Overview

**Goal:** 在 Phase 1~4 的骨架与存储层之上，按「P0 前置 → DAO → 上传 → 下载 → 分享/回收站/列表 → 定时任务 → 端到端 CLI 与降级演练」交付 Phase 5 业务层，并让两条硬约束（crc32Combine 对拍门槛、4MB 大帧 CRC 线程边界）成为可执行、可回退、可审计的验收项。

**Approach:** 先交付不依赖业务语义的底层能力（MD5、CRC combine、schema patch、CacheKey、Codec bulk、sendFile），再用 DAO 固化 SQL 与事务边界；上传/下载各自按「状态机 → 幂等 → 恢复 → 失败矩阵」推进；最后用 CLI 在 VM 上做杀进程续传与降级演练。每个 Stage 结束都必须能单独构建、单独回滚。

**Tech Stack:** 纯 C++11、CMake 3.5、GoogleTest 1.12.1（thirdparty/googletest，add_test 注册）、MySQL 5.7、Redis 3.2+、hiredis 0.13、libmysqlclient、Ubuntu 16.04 / g++ 5.4。

---

## 0. 两条硬约束（先读，后续任务不得绕过）

### H1：crc32Combine 必须先对拍，测试不全绿不得启用零拷贝默认路径

**交付顺序（不可交换）：**

1. 先实现 cm::crc32Combine(crc1, crc2, len2)，只作为纯函数交付，不接下载路径。
2. 同一次交付内写 common/tests/test_crc32_combine.cpp，随机对拍 **≥100 组**（计划 128 组），长度必须覆盖 **0、1、3、4MB-1**，并额外覆盖 4MB、1024、65535。
3. 对拍必须用**直接计算**做基准：对同一段 A||B 用 cm::Crc32 流式 update，结果与 combine(crc32(A), crc32(B), len(B)) 逐组比较。
4. 只有 ctest -R crc32_combine 全绿，才允许在下载阶段把 CM_DOWNLOAD_ZEROCOPY 的默认值从 OFF 翻到 ON。
5. 任一用例失败：保持 OFF，下载走缓冲读路径；零拷贝代码可以存在但不得成为默认路径，必须在计划状态里标记 Blocked 并记录失败用例编号。

**编译开关（P0 定义，D-0 启用）：**

~~~cmake
# cmake/options.cmake
option(CM_DOWNLOAD_ZEROCOPY "Use sendfile zero-copy block path" OFF)

# server/CMakeLists.txt（cm_logic 定义之后）
if(CM_DOWNLOAD_ZEROCOPY)
    target_compile_definitions(cm_logic PRIVATE CM_DOWNLOAD_ZEROCOPY=1)
else()
    target_compile_definitions(cm_logic PRIVATE CM_DOWNLOAD_ZEROCOPY=0)
endif()
~~~

**开关语义：**

| 取值 | 下载取块路径 | 使用场景 |
|---|---|---|
| OFF（P0 默认） | IO 池读 4MB + 流式 CRC + 普通 send | crc32Combine 未过门槛、sidecar 不可用、排障 |
| ON（D-0 之后默认） | 帧头 + sendfile payload；块 CRC 来自 sidecar | 正常生产路径 |

两条路径的**外部行为必须完全一致**：同样的 10013 请求返回同样的字节、同样的帧 CRC、同样的错误码。测试矩阵必须分别用 OFF 与 ON 各构建一次并跑同一组下载用例。

**对拍用例（固定清单）：**

| 用例 | 内容 | 期望 |
|---|---|---|
| Crc32CombineEmpty | A=空、B=空、len2=0 | 等于 crc32(空) |
| Crc32CombineKnownVector | "1234" + "56789" | 等于 0xCBF43926 |
| Crc32CombineRandomized128 | 128 组；前 8 组固定长度 0/1/2/3/1024/65535/4MB-1/4MB，其余随机 0~1MB；随机切分点 | 每组 combine == 直接流式 |
| Crc32CombineFourMbMinusOne | len(A)+len(B)=4MB-1 | 相等 |
| Crc32CombineHeader11 | A=11 字节帧头，B=4MB 块 | 等于 crc32(A||B) |
| Crc32CombineChunked | A/B 各自分 1/7/64KB 多段 update | 与一次性结果相同 |

随机数使用固定种子（0xC0FFEE），失败信息必须打印 case 序号、lenA、lenB，保证可复现。4MB 用例不要 128 组全跑 4MB，避免测试机内存与时间失控；固定长度表覆盖边界即可。

### H2：Codec 4MB 大帧 CRC 移交业务线程，payload 所有权必须转移，禁止 4MB/连接长期缓冲

**问题：** 现有 Codec::decode 在 IO 线程对 4MB payload 做 CRC（约 3~8ms），会阻塞同一 EventLoop 的所有连接；同时 Buffer 的 vector 容量只增不减，直接 decode 会让每条连接长期持有 4MB+ 容量。

**交付契约：**

~~~cpp
struct RawFrame {
    std::uint16_t type;
    std::uint32_t seq;
    std::uint32_t len;
    std::uint32_t expectedCrc;
    std::array<std::uint8_t, 11> header11;  // VERSION..SEQ
    std::string payload;                    // 拥有 payload，不指向 Buffer
};

DecodeStatus Codec::decodeBulk(Buffer& in, RawFrame* out);
bool Codec::verifyFrameCrc(const RawFrame& frame);
~~~

**所有权链（必须按这个顺序实现）：**

1. IO 线程 decodeBulk：校验 MAGIC/VERSION/LEN/类型，取出 11 字节头，in.retrieve(kHeaderSize)，再 out->payload = in.retrieveAsString(len)。
2. 这次提取是**一次 4MB 拷贝**（Buffer 是连续 vector，无法零拷贝切片）；拷贝完成后 Buffer 的读区被消费，payload 只属于 RawFrame。
3. 立刻调用 in.shrinkTo(kBufferRetainBytes)（kBufferRetainBytes=64KB）：保留未读的粘包数据，释放 4MB 级容量。禁止把 Buffer*、payload.data() 或指向 storage 的指针传给业务线程。
4. RawFrame 放进 std::shared_ptr<RawFrame>，用值捕获投递业务池（C++11 没有 init-capture，不能用 unique_ptr 捕获）。
5. 业务线程 verifyFrameCrc：失败→回 9005 + 请求 loop 关闭连接；成功→解析业务字段并处理。
6. 处理结束（成功或失败）后，shared_ptr 释放，4MB 内存归还 allocator；TcpConnection 不保留任何 payload 成员。

**同连接一次只允许一个未完成大帧（强制）：**

- 每条连接维护 bulkPending_（只在所属 loop 线程读写）与 bulkSeq_。
- decode 到 10006 且帧完整时：设置 bulkPending_=true，调用 conn->pauseRead()（必须设置 readPaused_ 标志，使 handleRead 尾部的 rearm 不再 enableReading），投递业务池，**立即停止本次 decode 循环**；即使 Buffer 里还有下一帧，也不得解析。
- 业务线程完成 CRC 校验与处理（成功或失败）后，必须 loop->runInLoop 回到连接线程：失败则 forceClose；成功则 bulkPending_=false、resumeRead()、继续 drain 未解析数据。
- 禁止：业务线程直接操作 Buffer；禁止在 CRC 未完成时对同一连接 decode 第二帧；禁止把 bulk 帧当普通帧继续循环。
- 若连接在等待校验期间关闭：业务线程持 shared_ptr<RawFrame> 继续完成校验（不访问连接），回 loop 时发现连接已断开则丢弃结果；不得 use-after-free。

**强制用例：**

| 用例 | 场景 | 期望 |
|---|---|---|
| BulkGateSecondFrameNotDecodedUntilVerified | 一个 Buffer 里放 10006（合法）+ 10007；CRC 校验用可控阻塞器卡住 | 10007 的 handler 调用次数在放行前为 0；放行后恰好 1 次 |
| BulkGateBadCrcClosesAndDropsNext | 10006 CRC 错 + 10007 | 回 9005 / forceClose；10007 永不投递 |
| BulkPayloadOwnedAfterBufferReset | 提取后 reset/reuse Buffer，再读 payload | payload 内容完整，不随 Buffer 变化 |
| BulkNoLongTerm4MbBuffer | 连续处理 10 个 4MB 帧 | 每次完成后 Buffer capacity ≤64KB（除未读粘包） |
| BulkShrinkPreservesPipelinedFrame | 4MB 帧后粘一个 10007 帧 | shrink 后 10007 字节完整，gate 放行后可解析 |
| BulkDecodeDoesNotVerifyCrc | 坏 CRC 的 10006 走 decodeBulk | 返回 Ok（交给业务线程验），verifyFrameCrc 返回 false |
| NormalDecodeStillVerifiesCrc | 10007 等普通帧 | 仍走原 decode，坏 CRC 返回 BadCrc（回归） |

**验收门槛：** ctest -R bulk_gate 全绿；ASan/LSan 下连续 100 个 4MB 帧无泄漏、无 4MB 常驻；grep 确认 TcpConnection 没有 payload 成员、业务线程代码没有 Buffer* 捕获。

---

## 1. 当前基线（计划必须建立在这上面）

已具备：

- Phase 1：单仓库骨架、cm_common / cm_protocol、build/bin 输出约定。
- Phase 2：Logger、Config、ErrorCode、Buffer、Crc32、协议 v2 Codec（decode 会同步校验 CRC）。
- Phase 3：EventLoop / Channel / EpollPoller / TcpConnection / TimerQueue / ThreadPool；TcpConnection 有 outputBuffer、8MB 高水位、无 sendFile。
- Phase 4：sql/schema_v2.sql、sql/patch_v2_1_index.sql、cm_storage（MysqlPool/MysqlConn/PreparedStatement）、cm_cache（RedisPool/CacheStore/CacheKey）。
- 测试：GTest 1.12.1 在 thirdparty/googletest，add_test 注册，ctest 可用。

尚不存在（本计划要补）：

- common/base/md5.*、crc32Combine。
- Buffer::capacity / shrinkTo；Codec::decodeBulk / verifyFrameCrc。
- TcpConnection::sendFile / pauseRead / resumeRead。
- cm_logic target、server/logic 下的任何服务与 handler。
- server/db 下的 UserDao / FileDao / TransferDao / ShareDao / PlayDao（Phase 4 只交付了池与语句封装）。
- sql/patch_v2_2_phase5.sql、6 位分享码的 CacheKey 校验、uploadReq/shareReq 缓存键。
- upload_cli / download_cli。
- 下载 sidecar、票据、令牌桶、块发送器。
- 上传合并、过期清理、孤儿 GC。

**重要现状差异：**

- 现有 Buffer::retrieveAsString 是“拷贝后消费”，且 Buffer 容量不释放；H2 必须新增 shrinkTo。
- 现有 Codec 的 Frame 只有 type/seq/payload，没有 header11/expectedCrc；decodeBulk 必须新增类型，不能改坏原 decode。
- 现有 TcpConnection::send 只接受内存字节；sendFile 必须处理“帧头先发、payload 后发”的顺序。
- CacheKey::share/shareMiss 目前硬编码 9 位数字；6 位分享码必须先改校验，否则分享缓存全部返回空 key。
- 现有 server/CMakeLists.txt 只定义 cm_storage、cm_cache、net、cloud-media-server、echo_server；cm_logic 与两个 CLI 要新增。

---

## 2. Files and Responsibilities

| 文件/模块 | 动作 | 职责 | 关键约束 |
|---|---|---|---|
| common/include/cm/base/md5.h、common/base/md5.cpp | Create | 流式 MD5（init/update/final/hex） | 纯 C++11，不引 OpenSSL；分片与一次性结果一致 |
| common/include/cm/base/crc32.h、common/base/crc32.cpp | Modify | 新增 crc32Combine(crc1, crc2, len2) | 与 zlib crc32_combine 等价；不改现有 Crc32 语义 |
| common/tests/test_md5.cpp | Create | MD5 已知向量、分块、4MB | 无外部依赖 |
| common/tests/test_crc32_combine.cpp | Create | H1 的 128 组对拍 | 边界长度必须覆盖 0/1/3/4MB-1 |
| common/include/cm/base/buffer.h、common/base/buffer.cpp | Modify | capacity()、shrinkTo(size_t) | 保留未读粘包；缩容后索引正确 |
| common/protocol/include/cm/protocol/frame.h | Modify | RawFrame 定义 | payload 拥有字节；header11 固定 11 字节 |
| common/protocol/include/cm/protocol/codec.h、codec.cpp | Modify | decodeBulk、verifyFrameCrc | 原 decode 不动；bulk 只允许白名单类型 |
| server/net/TcpConnection.h、TcpConnection.cpp | Modify | sendFile(prelude, fd, offset, len, cb)、pauseRead/resumeRead | fd 所有权归连接；prelude 必须先于 payload |
| server/net/tests/test_send_file.cpp | Create | sendfile 整文件/部分写/回调一次/fd 关闭 | Linux；Windows 用 GTEST_SKIP |
| server/logic/connection_decoder.h、connection_decoder.cpp | Create | 每连接 decode 循环 + bulkPending 门闩 | 只在 loop 线程访问状态 |
| server/logic/tests/test_bulk_gate.cpp | Create | H2 的 7 个门闩用例 | 可控阻塞 CRC 校验器 |
| sql/patch_v2_2_phase5.sql | Create | 分享码 CHAR(6)、idx_deleted、状态/offset 注释 | 不建库、不 USE；人工执行 |
| server/tests/storage/test_schema_v2_2.cpp | Create | schema 契约 | 读 information_schema |
| server/cache/cache_store.h、cache_store.cpp | Modify | CacheKey::share 6 位、uploadReq/shareReq | 保留旧 key 前缀 |
| common/include/cm/base/error_code.h、common/base/error_code.cpp | Modify | 3007 TaskExpired、3008 TooManyNodes | to_string/is_retryable 同步 |
| common/protocol/message_type.h | Modify | 10011/10024 复用 + 10030~10043 | 与 06 第 3 节逐项一致 |
| common/protocol/messages/*.h | Create | 上传/下载/分享/回收站字段序列化 | 只依赖 Writer/Reader |
| server/db/user_dao.* | Create | 登录/注册最小 DAO | 预处理语句；不拼 SQL |
| server/db/file_dao.* | Create | blob + 用户节点查询/插入/软删/恢复/分页 | 事务边界与 06 一致 |
| server/db/transfer_dao.* | Create | 任务、分片、位图、选主、过期扫描 | 行锁顺序：任务→分片 |
| server/db/share_dao.* | Create | 分享创建/查询/列表/取消 | 1062 重试由服务层做 |
| server/db/play_dao.* | Create | 播放进度 upsert | LWW |
| server/logic/upload_service.* | Create | 初始化/秒传/分片/续传/合并/过期 | 合并走 IO 池 |
| server/logic/download_service.* | Create | 打开/票据/取块/限速/续传 | 两条块路径由开关选择 |
| server/logic/share_service.* | Create | 6 位码、有效期、提取码、缓存 | fail-open/closed 按 06 第 10 节 |
| server/logic/recycle_service.* | Create | 软删/恢复/彻底删除/子树 | 计数唯一例外按 06 第 8.1 |
| server/logic/file_service.* | Create | 列表/游标/计数 | 禁止 OFFSET |
| server/logic/play_service.* | Create | 播放上报 | 无缓存 |
| server/logic/block_crc_sidecar.* | Create | .crc 读写/懒填充 | 与 blob 同生命周期 |
| server/logic/download_ticket_cache.* | Create | ticket 签发/校验/滑动续期 | Redis 挂 fail-closed |
| server/logic/rate_limiter.* | Create | 连接/用户令牌桶 | 不阻塞 loop |
| server/logic/jobs/expiry_job.* | Create | 上传过期扫描 | 5min/200 |
| server/logic/jobs/orphan_gc_job.* | Create | 孤儿 blob GC | 24h 宽限/100 行/2GB |
| server/logic/jobs/share_expiry_job.* | Create | 分享过期扫描 | 每日 1000 |
| server/app/server_app.* | Create | 接线业务池/IO 池/定时任务 | 当前只有 main.cpp；退出顺序不变 |
| server/CMakeLists.txt | Modify | cm_logic、upload_cli、download_cli、GTest | cmake 3.5 语法 |
| server/examples/upload_cli.cpp | Create | 上传 CLI | -c/-r/--md5/--stop-after-chunks |
| server/examples/download_cli.cpp | Create | 下载 CLI | -c/-j/-r/--md5/--stop-after-blocks |
| server/logic/tests/*.cpp | Create | 服务层单测/集成测试 | 真实 DB 用环境变量开关 |
| docs/benchmark/phase5_zero_copy_gate.md | Create | H1 门槛记录 | 记录日期、128 组、全绿结论 |
| docs/benchmark/phase5_e2e.md | Create | CLI 杀进程/续传/降级记录 | 不写密码 |

---

## 3. Assumptions and Dependencies

- 目标机 Ubuntu 16.04 / g++ 5.4 / cmake 3.5；CMake 不使用 3.6+ 命令；测试用 add_test，不用 gtest_discover_tests。
- MySQL 5.7 已开 innodb_large_prefix，表为 ROW_FORMAT=DYNAMIC；Redis 3.2+；hiredis 0.13 无 redisReconnect。
- 真实 DB/Redis 集成测试默认不跑：CM_TEST_MYSQL=1 / CM_TEST_REDIS=1 时才连接；未设置时 GTEST_SKIP。
- 测试凭据只从环境变量读取（CM_TEST_MYSQL_PASSWORD / CM_TEST_REDIS_PASSWORD），仓库与 CMake 不出现密码。
- 上传/下载的 4MB 常量固定为 4194304；客户端 CLI 传其他 -c 值时服务端仍按 4MB 协商并在输出里提示。
- 旧 blob 没有 .crc sidecar 时允许懒生成；懒生成失败不阻断下载，走缓冲路径。
- 06 文档的 4 项冲突裁决视为已批准；若评审推翻分享码 6 位或 10011/10024 复用，P0-3/P0-4 与消息表需要重做，属于 Blocking 变更。
- 本阶段不实现转存、目录打包下载、HLS；不引入 C++14/17。
- 不做 commit 步骤；用户未要求提交。

---

## 4. Task Map（严格按此顺序）

| Stage | 任务 | 交付物 | 通过门槛 |
|---|---|---|---|
| P0 | P0-1 Md5 | 流式 MD5 + 测试 | 已知向量/分块/4MB 全绿 |
| P0 | P0-2 crc32Combine + H1 门槛 | combine + 128 组对拍 + CMake 开关 | ctest -R crc32_combine 全绿；否则开关保持 OFF |
| P0 | P0-3 patch_v2_2 | schema patch + 契约测试 | 分享码 CHAR(6)、idx_deleted 生效 |
| P0 | P0-4 CacheKey | 6 位分享码 + req 键 | 新旧 key 行为测试全绿 |
| P0 | P0-5 CodecBulk + H2 门闩 | RawFrame/decodeBulk/shrink/门闩 | bulk_gate 7 例全绿；无 4MB 常驻 |
| P0 | P0-6 sendFile | prelude+sendfile+pauseRead | send_file 7 例全绿；fd 不泄漏 |
| DAO | DAO-1~5 | 5 个 DAO + 测试夹具 | 无 DB 单测 + 有 DB 集成全绿 |
| Upload | U-1~U-6 | 消息、初始化、分片、续传、合并、恢复 | U1~U11 对应服务层用例全绿 |
| Download | D-0~D-6 | 门槛翻转、sidecar、票据、两条块路径、限流、服务 | OFF/ON 两套构建全绿；D1~D6 全绿 |
| Share/Recycle/List | S-1~S-4 | 分享、回收站、列表、播放 | S1~S5/R1~R4/P1~P2 全绿 |
| Timers | T-1~T-3 | 过期、GC、分享扫描 | 批量/幂等/宽限用例全绿 |
| E2E | E-1~E-5 | 两个 CLI、杀进程、降级演练、文档 | VM 上 50MB 文件 MD5 一致；降级行为符合表 |

---

## Stage P0：前置能力（任何业务代码之前）

### Task P0-1：流式 MD5

**Goal:** 提供 cm::Md5，支持分块 update 与一次性 hex，供上传合并与客户端整文件校验复用。

**Files:**
- Create: common/include/cm/base/md5.h
- Create: common/base/md5.cpp
- Create: common/tests/test_md5.cpp
- Modify: common/CMakeLists.txt（加入 base/md5.cpp）
- Modify: common/tests/CMakeLists.txt（注册 md5 测试可执行文件）

**Steps:**
- [ ] 定义 class Md5 { void update(const void*, size_t); std::string hex(); void reset(); } 与 std::string md5Hex(const void*, size_t)；不引 OpenSSL，不依赖平台。
- [ ] 实现标准 MD5（RFC 1321）：小端 32 位累加、64 字节块、末尾 0x80 + 长度；hex 输出 32 位小写。
- [ ] 单测覆盖：空串、abc、123456789、1KB 随机、4MB-1、4MB；同一数据分 1/7/64KB 多段 update 与一次性结果一致。
- [ ] 单测覆盖 reset 后复用；update(NULL,0) 不崩溃。

**gtest 清单:**
- Md5Empty："" → d41d8cd98f00b204e9800998ecf8427e
- Md5KnownVectors：abc → 900150983cd24fb0d6963f7d28e17f72；123456789 → 25f9e794323b453885f5181f1b624d0b
- Md5ChunkedEqualsOneShot：0/1/3/4MB-1
- Md5FourMb：4MB 随机数据，分块与一次性一致
- Md5ResetReuse：同一对象 reset 后结果正确

**验收标准:**
- [ ] ctest -R md5 全绿。
- [ ] 4MB 用例耗时 <2s（Debug 下放宽到 5s），不出现 O(n²) 拷贝。
- [ ] 文件无 C++14/17 特性，-Wall -Wextra 零警告。

**风险:**
- MD5 实现位运算写错 → 用 3 组已知向量 + 固定期望的随机对拍兜底。
- 大文件一次性读入内存 → 单测用 4MB 上限，生产合并按 1MB 缓冲流式 update，禁止整文件读入。

---

### Task P0-2：crc32Combine + H1 对拍门槛

**Goal:** 交付与 zlib crc32_combine 等价的纯函数，并用 128 组随机对拍锁死正确性；在门槛未过前，零拷贝默认路径保持关闭。

**Files:**
- Modify: common/include/cm/base/crc32.h
- Modify: common/base/crc32.cpp
- Create: common/tests/test_crc32_combine.cpp
- Modify: cmake/options.cmake（新增 CM_DOWNLOAD_ZEROCOPY，默认 OFF）
- Modify: common/tests/CMakeLists.txt（注册 crc32_combine 测试可执行文件）
- Create: docs/benchmark/phase5_zero_copy_gate.md（记录门槛结果）

**Steps:**
- [ ] 在 crc32.h 增加 std::uint32_t crc32Combine(std::uint32_t crc1, std::uint32_t crc2, std::uint64_t len2);，语义为 CRC(A||B)，输入是已 finalize 的 CRC。
- [ ] 用 GF(2) 矩阵平方实现 combine（与 zlib 算法一致）；不引入 zlib 依赖；len2 为 0 时直接返回 crc1。
- [ ] 实现 128 组参数化对拍：固定种子 0xC0FFEE；长度表覆盖 0/1/2/3/1024/65535/4MB-1/4MB；其余在 0~1MB 随机；切分点随机。
- [ ] 每组同时计算 crc32(A)、crc32(B)、combine(...) 与 Crc32 流式 update(A); update(B)，断言四者关系。
- [ ] 增加 header11 用例：A 为 11 字节帧头，B 为 4MB 块，验证下载帧 CRC 的合成方式。
- [ ] 在 options.cmake 定义 option(CM_DOWNLOAD_ZEROCOPY ... OFF)；此时不接任何业务路径。
- [ ] 记录门槛：docs/benchmark/phase5_zero_copy_gate.md 写日期、用例数、命令文本、全绿输出摘要；不写“大概通过”。

**gtest 清单:**
- Crc32CombineEmpty
- Crc32CombineKnownVector
- Crc32CombineRandomized128（≥100 组，含 0/1/3/4MB-1）
- Crc32CombineFourMbMinusOne
- Crc32CombineFourMb
- Crc32CombineHeader11
- Crc32CombineChunked
- Crc32CombineLenZeroIsCrc1

**验收标准:**
- [ ] ctest -R crc32_combine 全绿，输出明确显示 128 组。
- [ ] 直接计算与 combine 的 128 组结果 0 差异；失败时打印 case/lenA/lenB/期望/实际。
- [ ] CM_DOWNLOAD_ZEROCOPY 默认 OFF；在 OFF 下构建与下载测试可用。
- [ ] 门槛记录文件已写，且明确“未全绿不得翻 ON”。

**风险:**
- 多项式/位序写反 → 用已知向量与 header11 用例双重校验；先实现 len2=0/1 的退化用例。
- 128 组里多次 4MB 分配导致测试机 OOM → 固定长度表只放少量 4MB，其余小长度；单组用完立即释放。
- 误把 combine 结果再 xor → 明确输入/输出都是 finalize 值，单测锁死。

---

### Task P0-3：schema patch v2_2 与契约测试

**Goal:** 把 06 第 1.4 节的 DDL 变更落成独立 patch，并用 information_schema 契约测试防止漂移。

**Files:**
- Create: sql/patch_v2_2_phase5.sql
- Create: server/tests/storage/test_schema_v2_2.cpp
- Modify: server/tests/CMakeLists.txt（加入契约测试）

**Steps:**
- [ ] patch 内容严格为：t_share.s_code CHAR(6) ascii、t_user_file 新增 idx_deleted(u_id, uf_deleted_at)、t_transfer_task 的状态注释含 7、tr_offset 注释改为“已完成分片字节总和”。
- [ ] 不写 CREATE DATABASE / USE；不删旧索引；不改其他列。
- [ ] 契约测试连接 CM_TEST_MYSQL 指定的库，查 information_schema.COLUMNS 与 STATISTICS：
  - s_code 的 CHARACTER_MAXIMUM_LENGTH=6；
  - idx_deleted 存在且列顺序为 u_id, uf_deleted_at；
  - tr_status 的 COLUMN_COMMENT 含“7合并中”；
  - tr_offset 的 COLUMN_COMMENT 含“已完成分片字节总和”。
- [ ] 未设置 CM_TEST_MYSQL 时 GTEST_SKIP，不失败。

**gtest 清单:**
- SchemaV22ShareCodeLength
- SchemaV22IdxDeleted
- SchemaV22StatusComment
- SchemaV22OffsetComment
- SchemaV22NoForeignKeyOrTrigger（回归：仍然 0 个外键/触发器）

**验收标准:**
- [ ] 人工在测试库执行 patch 一次成功；重复执行会因列已存在报错，属于预期（不做成幂等，避免掩盖漂移）。
- [ ] 契约测试在有 DB 环境全绿；无 DB 环境 5 个 SKIP。
- [ ] 旧 schema_v2.sql 不被改写；patch 可单独审查。

**风险:**
- 线上已有 9 位分享码数据 → 本阶段是全新库/开发库，patch 前必须确认无旧数据；迁移策略不在本期。
- CHAR(6) 与 ascii 字符集不一致 → patch 显式写 CHARACTER SET ascii，契约测试查 charset。
- 索引名冲突 → 先查 STATISTICS，存在则人工确认后再执行。

---

### Task P0-4：CacheKey 6 位分享码与幂等键

**Goal:** 让 CacheKey 支持 6 位分享码，并新增 uploadReq/shareReq 两个幂等键；旧 9 位行为明确废弃。

**Files:**
- Modify: server/cache/cache_store.h
- Modify: server/cache/cache_store.cpp
- Modify: server/tests/storage/test_cache_key.cpp（若不存在则 Create）
- Modify: server/tests/CMakeLists.txt

**Steps:**
- [ ] CacheKey::share/shareMiss 改为：长度必须 6；字符属于 23456789ABCDEFGHJKMNPQRSTUVWXYZ；不接受小写（服务层归一化后再调用）。
- [ ] 新增 uploadReq(uid, reqId) → cm:v1:up:req:{uid}:{reqId}；shareReq(uid, reqId) → cm:v1:share:req:{uid}:{reqId}。
- [ ] 保留 session/user/fileByMd5/fileById/rateLimitIp/rebuildLock/downloadTicket 不变。
- [ ] 非法 key 返回空串（沿用现有约定），调用方必须检查空串，不得把空 key 发给 Redis。
- [ ] 单测覆盖：合法 6 位、旧 9 位拒绝、含 0/O/1/I/L 拒绝、小写拒绝、req 键唯一、空/超长拒绝。

**gtest 清单:**
- CacheKeyShareSixChars
- CacheKeyShareRejectsNineDigits
- CacheKeyShareRejectsConfusableChars
- CacheKeyShareRejectsLowercase
- CacheKeyUploadReq
- CacheKeyShareReq
- CacheKeyRejectsEmptyAndTooLong

**验收标准:**
- [ ] ctest -R cache_key 全绿。
- [ ] 旧 9 位调用返回空串，不会静默命中错误 key。
- [ ] 代码中不存在第二套 share key 拼装。

**风险:**
- 现有调用方假设 9 位 → grep 全部 CacheKey::share 调用点，逐一改为先归一化再调用；本阶段调用方只有测试，Phase 5 服务层新增。
- req_id 溢出/回绕 → 用 u32 十进制字符串，允许重复但 TTL 10min；客户端保证单调。

---

### Task P0-5：CodecBulk、Buffer 缩容与同连接门闩（H2）

**Goal:** 把 4MB 帧的 CRC 校验移出 IO 线程，同时保证 payload 所有权清晰、Buffer 不长期占用 4MB、CRC 未完成时同连接不解下一帧。

**Files:**
- Modify: common/protocol/include/cm/protocol/frame.h
- Modify: common/protocol/include/cm/protocol/codec.h
- Modify: common/protocol/codec.cpp
- Modify: common/include/cm/base/buffer.h
- Modify: common/base/buffer.cpp
- Create: server/logic/connection_decoder.h
- Create: server/logic/connection_decoder.cpp
- Create: server/logic/tests/test_bulk_gate.cpp
- Modify: common/protocol/tests/test_codec.cpp（回归）
- Modify: common/tests/CMakeLists.txt（Codec/Buffer 回归）、server/tests/CMakeLists.txt（bulk_gate 独立测试）

**Steps:**
- [ ] 在 frame.h 增加 RawFrame（type/seq/len/expectedCrc/header11/payload）；header11 用 std::array<uint8_t,11>。
- [ ] Buffer 增加 capacity() 与 shrinkTo(size_t minCapacity)：保留未读数据（readerIndex..writerIndex），重新分配并复制；minCapacity 至少 1024，最大不超过原容量。
- [ ] 实现 Codec::decodeBulk(Buffer&, RawFrame*, DecodeStatus*)：
  - 校验 MAGIC/VERSION/LEN/类型白名单（仅 10006；其他类型返回 DecodeStatus::UnknownType，且不消费 Buffer，readableBytes 不变）；
  - 复制 header11；in.retrieve(kHeaderSize)；out->payload = in.retrieveAsString(len)；不校验 CRC；
  - 调用 in.shrinkTo(64*1024)；返回 Ok/NeedMore/BadMagic/BadVersion/BadLength/UnknownType。
- [ ] 实现 Codec::verifyFrameCrc(const RawFrame&)：用 cm::Crc32 update header11 的 11 字节 + payload，比较 expectedCrc。
- [ ] 实现 ConnectionDecoder：
  - 普通帧循环 decode，直到 NeedMore；
  - 遇到 10006：构造 shared_ptr<RawFrame>，设置 bulkPending_，pauseRead，投业务池，返回 StopForBulk；
  - 业务线程 verify：失败回 loop forceClose；成功调用 handler；完成后回 loop 清 bulkPending_、resumeRead、继续 drain；
  - 状态只在 loop 线程读写；业务线程不持有 Buffer*、不持有 TcpConnection 引用（用 weak_ptr 回投）。
- [ ] 单测使用可控阻塞器：验证第二帧在放行前不解析；坏 CRC 时第二帧不投递；payload 在 Buffer reset 后仍完整；连续 10 个 4MB 帧后 capacity ≤64KB；粘包保留。

**gtest 清单:**
- CodecBulkParsesHeaderWithoutCrc
- CodecBulkBadMagicVersionLength
- CodecBulkRejectsNonBulkType
- CodecBulkPayloadOwnedAfterBufferReset
- BufferShrinkPreservesPipelinedFrame
- BufferCapacityDropsAfterBulk
- BulkGateSecondFrameNotDecodedUntilVerified
- BulkGateBadCrcClosesAndDropsNext
- BulkNoLongTerm4MbBuffer
- NormalDecodeStillVerifiesCrc

**验收标准:**
- [ ] ctest -R 'bulk_gate|cm_common_tests' 全绿（bulk_gate 为独立 add_test 名，原 Codec/Buffer 回归在 cm_common_tests 内）。
- [ ] BulkGateSecondFrameNotDecodedUntilVerified 在阻塞器放行前断言 handler 计数 0；放行后 1。
- [ ] 连续 10 个 4MB 帧后，TcpConnection/Buffer 没有 4MB 常驻；ASan/LSan 无泄漏。
- [ ] grep 确认业务线程代码没有 Buffer* 捕获、TcpConnection 没有 payload 成员。
- [ ] 原 Codec::decode 的 8 个回归用例仍全绿。

**风险:**
- 门闩死锁：业务池满/任务丢失导致 bulkPending 永不清 → 连接永久停读。对策：handler 必须在 RAII 作用域结束时无条件回投；加 5s 超时 watchdog，超时 forceClose 并记 ERROR。
- pauseRead 降低吞吐 → 4MB/帧 + 单帧在途；LAN 足够。若 Phase 7 压测显示 RTT 受限，再评估“允许 1 帧预读”，但不得改成无限缓冲。
- shrinkTo 复制粘包数据 → 只在 readable>0 时分配 minCapacity，最坏复制下一帧；单测锁死。
- 4MB 拷贝峰值 8MB → 记录在风险与内存预算里；不允许再加整帧副本。

---

### Task P0-6：sendFile、pauseRead/resumeRead

**Goal:** 给 TcpConnection 增加可取消的 sendfile 发送能力，并保证帧头先于 payload、fd 所有权清晰、EAGAIN 不阻塞 loop。

**Files:**
- Modify: server/net/TcpConnection.h
- Modify: server/net/TcpConnection.cpp
- Create: server/net/tests/test_send_file.cpp
- Modify: server/net/tests/CMakeLists.txt

**Steps:**
- [ ] 增加 bool sendFile(const std::string& prelude, int fd, off_t offset, size_t len, std::function<void(bool)> done);：
  - 连接接管 fd 所有权（完成/失败/关闭都 close）；
  - prelude 先 append 到 outputBuffer；只有 outputBuffer 写空后才启动 sendfile；
  - 每轮最多 1MB；EAGAIN 时保留剩余区间并 enableWriting；
  - 完成回调只调用一次（成功 true / 失败 false）；
  - 连接关闭时取消并 close fd，回调 false。
- [ ] 增加 pauseRead() / resumeRead()：pauseRead 设置 readPaused_ 并 disableReading；rearm() 在 readPaused_ 时直接返回，防止消息回调尾部把 EPOLLIN 又打开；resumeRead 清标志并 enableReading。只改本 loop 的 EPOLLIN，不影响写；关闭状态下无副作用。
- [ ] Windows 编译占位：sendFile 返回 false 并 close fd；pauseRead/resumeRead 空实现。
- [ ] 单测：4MB 临时文件整发；prelude 先到；SO_SNDBUF 很小造成部分写；回调一次；完成后 fd 已关闭；关闭连接时 fd 关闭且回调 false。

**gtest 清单:**
- TcpConnectionSendFileWholeFile
- TcpConnectionSendFilePreludeFirst
- TcpConnectionSendFilePartialEagain
- TcpConnectionSendFileCallbackOnce
- TcpConnectionSendFileFdClosedOnComplete
- TcpConnectionSendFileCancelOnClose
- TcpConnectionPauseReadStopsEpollIn
- TcpConnectionPauseReadSurvivesRearm（在消息回调内 pause，回调返回后仍不读）

**验收标准:**
- [ ] ctest -R send_file 全绿（Linux）；Windows 全部 SKIP。
- [ ] 4MB 传输字节与文件逐字节一致；prelude 在前。
- [ ] 关闭连接后回调不悬挂，fd 数量回到基线（/proc/self/fd 对比）。
- [ ] 空闲时无 EPOLLOUT 常驻，CPU 不空转。

**风险:**
- fd 所有权不清导致 double close → API 文档写死“连接接管”，单测用 fcntl 检查关闭。
- prelude 未发完就 sendfile → 必须等 outputBuffer 空；单测用小 SNDBUF 放大该窗口。
- 磁盘读阻塞 loop → 本阶段接受；Phase 7 用 fadvise/缓冲回退，监控 loop 延迟。

---

## Stage DAO：SQL 与事务边界

### Task DAO-1：UserDao

**Goal:** 提供登录/注册所需的最小查询与插入，作为 DAO 模式的基线。

**Files:**
- Create: server/db/user_dao.h
- Create: server/db/user_dao.cpp
- Create: server/tests/logic/test_user_dao.cpp
- Modify: server/tests/CMakeLists.txt

**Steps:**
- [ ] 定义 UserRow（u_id/u_tel/u_password/u_salt/u_pwd_algo/u_name/u_status）。
- [ ] findByTel(tel, UserRow*)；insert(...)；updatePassword(uid, hash, algo, salt)。
- [ ] 全部 PreparedStatement；INSERT 捕获 1062 返回 AlreadyExists。
- [ ] 单测：找不到返回 NotFound；插入后可查；重复 tel 返回 AlreadyExists；密码字段不打印。

**gtest 清单:**
- UserDaoFindMissing
- UserDaoInsertAndFind
- UserDaoDuplicateTel
- UserDaoUpdatePassword

**验收标准:**
- [ ] 有 DB 集成测试全绿；无 DB SKIP。
- [ ] 慢查询日志只含占位符 SQL，不含密码。
- [ ] 无字符串拼接 SQL。

**风险:**
- 测试数据污染 → 每个用例用唯一 tel 前缀 + 测试后清理；不 TRUNCATE 全表。
- 事务缺失 → 单条写不需要事务；更新密码单条即可。

---

### Task DAO-2：FileDao（blob + 用户节点）

**Goal:** 固化 t_file 与 t_user_file 的查询/插入/软删/恢复/分页 SQL，锁定引用计数与自动改名规则。

**Files:**
- Create: server/db/file_dao.h
- Create: server/db/file_dao.cpp
- Create: server/tests/logic/test_file_dao.cpp

**Steps:**
- [ ] 方法：findBlobByMd5、insertBlobOnDuplicateIncrement、getBlobById、createUserFileAutoRename、listByParent（三种排序 + 游标）、softDelete、restore、purgeNode、findUserFile、countByParent。
- [ ] createUserFileAutoRename 在事务内尝试原名与 (2)…(99)，捕获 1062 继续；返回最终名。
- [ ] softDelete 在同一事务内减 ref_count（条件 >0）并在 0 时置 f_state=2。
- [ ] 列表 SQL 用行构造器游标，禁止 OFFSET；limit+1 判断 has_more。
- [ ] 单测：同名自动改名、软删只减一次、恢复加一次、游标同微秒稳定。

**gtest 清单:**
- FileDaoFindBlobByMd5
- FileDaoInsertBlobIncrementsRef
- FileDaoAutoRename
- FileDaoSoftDeleteDecrementsOnce
- FileDaoRestoreIncrementsOnce
- FileDaoListNameCursor
- FileDaoListSameMicrosecond
- FileDaoNoOffsetSql（静态检查/字符串断言）

**验收标准:**
- [ ] 有 DB 集成全绿；引用计数轨迹 -1/+1 正确。
- [ ] 分页 1000 行、每页 50 无重复/漏行。
- [ ] SQL 中无 LIMIT ?,? 形式的大偏移。

**风险:**
- 自动改名在事务内 99 次重试 → 最坏 99 条语句；实际冲突极少，设置上限并返回 1003。
- 唯一键冲突是语句级回滚，事务可继续；必须用 MySQL 实测确认，不凭假设。

---

### Task DAO-3：TransferDao（任务 + 分片）

**Goal:** 固化上传任务状态机 SQL、分片幂等 upsert、位图查询、合并选主与过期扫描。

**Files:**
- Create: server/db/transfer_dao.h
- Create: server/db/transfer_dao.cpp
- Create: server/tests/logic/test_transfer_dao.cpp

**Steps:**
- [ ] 方法：createTask、findTaskByTaskId、lockTaskForUpdate、upsertChunkIfAbsent（返回是否新完成）、listDoneIndexes、sumDoneBytes、markStatus、electMerge（status 1→7）、resetMergingOnStartup、findExpired。
- [ ] 所有状态更新用条件 UPDATE（WHERE tr_status IN (...))，返回 affected_rows。
- [ ] 分片 upsert 必须区分 insert/update/no-op，确保 done_chunks 只在新完成时 +1。
- [ ] 位图查询按 c_index 升序，LSB-first 由服务层组装。
- [ ] 单测：重复分片不增计数；乱序计数正确；electMerge 并发只有一个 affected=1；过期扫描只取 0/1/2。

**gtest 清单:**
- TransferDaoCreateAndFind
- TransferDaoUpsertChunkNew
- TransferDaoUpsertChunkDuplicate
- TransferDaoUpsertChunkRewriteState0
- TransferDaoDoneIndexesBitmap
- TransferDaoElectMergeOnce
- TransferDaoExpiredScan
- TransferDaoResetMerging

**验收标准:**
- [ ] 有 DB 集成全绿；两个线程同时 electMerge 只有一个成功。
- [ ] 重复分片后 tr_done_chunks 不增、tr_offset 不增。
- [ ] 事务内锁顺序为任务行→分片行（代码审查 + 测试并发）。

**风险:**
- ON DUPLICATE KEY UPDATE 的 affected_rows 语义（1/2/0）易误判 → 用 SELECT ... FOR UPDATE 显式读旧状态，或单测锁死三种返回值。
- 计数缓存漂移 → 续传时以 t_chunk 重算并修正。

---

### Task DAO-4：ShareDao + PlayDao

**Goal:** 固化分享创建/查询/列表/取消与播放进度 upsert。

**Files:**
- Create: server/db/share_dao.h
- Create: server/db/share_dao.cpp
- Create: server/db/play_dao.h
- Create: server/db/play_dao.cpp
- Create: server/tests/logic/test_share_dao.cpp
- Create: server/tests/logic/test_play_dao.cpp

**Steps:**
- [ ] ShareDao：insert（捕获 1062 由服务层重试）、findByCode、listByUser（游标）、cancel、listActiveByUfId、sweepExpired。
- [ ] PlayDao：upsertLww（position/duration/finished/updated_at=NOW(6)）、findByUserFile、listRecent。
- [ ] 分享码列按 CHAR(6) 读写；s_file_id 目录为 NULL。
- [ ] 单测：重复 code 插入 1062；取消幂等；播放 upsert 不产生第二行；LWW 覆盖旧值。

**gtest 清单:**
- ShareDaoInsertDuplicateCode
- ShareDaoFindByCode
- ShareDaoListCursor
- ShareDaoCancelIdempotent
- ShareDaoSweepExpired
- PlayDaoUpsertLww
- PlayDaoOneRowPerUserFile

**验收标准:**
- [ ] 有 DB 集成全绿。
- [ ] 分享列表按 s_created_at DESC, s_id DESC 稳定。
- [ ] 播放表唯一键 (p_user_id,p_file_id) 生效。

**风险:**
- 分享码生成冲突由服务层重试 5 次；DAO 只负责抛 1062，不吞错。
- 播放进度回退是合法产品行为，不能用 MAX 覆盖。

---

### Task DAO-5：DAO 测试夹具与数据清理

**Goal:** 提供可重复的测试库夹具，避免每个 DAO 用例重复造用户/blob/节点。

**Files:**
- Create: server/tests/logic/dao_fixture.h
- Create: server/tests/logic/dao_fixture.cpp
- Create: server/tests/logic/CMakeLists.txt
- Modify: server/tests/CMakeLists.txt（add_subdirectory(logic)）

**Steps:**
- [ ] 定义 cm_logic_tests（初始包含 DAO 四个测试源，链接 cm_storage/cm_cache/GTest；后续 Stage 把服务层测试源加入同一 target），并用 add_test(NAME logic_unit ...) 注册。
- [ ] Fixture 在 SetUp 检查 CM_TEST_MYSQL，未设置则 GTEST_SKIP。
- [ ] 提供 createUser、createBlob、createNode、createTask、createShare 辅助函数，返回自增 ID。
- [ ] 每个用例使用唯一前缀（pid + 时间 + 序号），TearDown 按前缀清理；禁止 TRUNCATE。
- [ ] 测试失败时打印 SQL 占位符与 ID，不打印密码/token。

**gtest 清单:**
- DaoFixtureSmoke（创建/清理后计数回到基线）
- DaoFixtureUniquePrefix

**验收标准:**
- [ ] 连续跑两遍 DAO 测试，第二遍不因残留数据失败。
- [ ] 清理 SQL 只删本前缀数据。

**风险:**
- 清理遗漏导致唯一键冲突 → 用唯一前缀而非固定 tel/md5；TearDown 尽力清理并记录。

---

## Stage Upload：初始化、分片、续传、合并

### Task U-1：业务消息与序列化（10004~10043）

**Goal:** 落地 06 第 3/4 节全部业务消息的字段编解码，包括上传、下载、分享、回收站、列表、播放上报；同时补 3007/3008 错误码。

**Files:**
- Modify: common/protocol/include/cm/protocol/message_type.h
- Create: common/protocol/include/cm/protocol/messages/upload.h
- Create: common/protocol/include/cm/protocol/messages/download.h
- Create: common/protocol/include/cm/protocol/messages/share.h
- Create: common/protocol/include/cm/protocol/messages/recycle.h
- Create: common/protocol/include/cm/protocol/messages/list.h
- Create: common/protocol/include/cm/protocol/messages/play.h
- Create: common/protocol/messages/business_messages.cpp
- Create: common/protocol/tests/test_business_messages.cpp
- Modify: common/include/cm/base/error_code.h、common/base/error_code.cpp（3007/3008）
- Modify: common/tests/test_error_code.cpp（新码唯一性/可重试性）
- Modify: common/CMakeLists.txt、common/tests/CMakeLists.txt

**Steps:**
- [ ] 类型号按 06 第 3 节：10011→DownloadDataRs、10024→QuickUploadRs、新增 10030~10043；message_type.h 与枚举 isKnownType 同步。
- [ ] 为 10004~10043 每个消息实现 encode/decode（Writer/Reader），字段顺序与 06 第 4 节逐项一致；10011 是纯 raw payload，不实现字段解析。
- [ ] 校验字符串上限、md5 32 hex、bitmap 长度、bad_indexes 数量上限（≤1024）、权限码/策略枚举取值。
- [ ] 新增 3007 TaskExpired、3008 TooManyNodes；to_string/is_retryable 同步。
- [ ] 单测：每类消息正常往返、截断、超长字符串、错误 md5、bitmap 长度不符、非法枚举。

**gtest 清单:**
- UploadInitRoundTrip
- UploadChunkRoundTrip
- ContinueUploadBitmapRoundTrip
- UploadCompleteBadIndexesRoundTrip
- DownloadOpenRoundTrip
- DownloadStatRoundTrip
- ShareCreateRoundTrip
- ShareOpenRoundTrip
- RecycleListRoundTrip
- RestoreRoundTrip
- PurgeRoundTrip
- ListFileRoundTrip
- PlayReportRoundTrip
- BusinessMessageRejectsBadMd5
- BusinessMessageRejectsTruncated
- BusinessMessageRejectsBitmapTooLong
- ErrorCodeNewValuesUnique

**验收标准:**
- [ ] ctest -R upload_messages 全绿。
- [ ] 序列化结果与 06 字段表逐项一致（review 对照）。
- [ ] 不依赖 Qt/FFmpeg。

**风险:**
- 10011/10024 复用与 02 旧表冲突 → 以 06 附录 A 为准；02 修订列入收尾。
- bad_indexes 无限增长 → 上限 1024，超出截断并置 1001。

---

### Task U-2：UploadInit + 秒传 + req 幂等

**Goal:** 实现 10004：新任务、秒传命中落库、req_id 结果缓存、自动改名。

**Files:**
- Create: server/logic/upload_service.h
- Create: server/logic/upload_service.cpp
- Create: server/logic/tests/test_upload_init.cpp
- Modify: server/CMakeLists.txt（新增 cm_logic STATIC）

**Steps:**
- [ ] 在 server/CMakeLists.txt 新增 cm_logic STATIC（先只含 upload_service.cpp），PUBLIC 链接 cm_protocol/cm_storage/cm_cache，PRIVATE 链接 net 与 Threads；后续任务把新源文件加入同一 target。
- [ ] 参数校验；parent 必须属于当前用户且是目录。
- [ ] req 幂等：读 cm:v1:up:req:{uid}:{reqId}；未命中用 SET NX PX 5000 短锁；完成后写结果 10min。
- [ ] 秒传：Redis file:md5 → DB uk_f_md5 → 只认 f_state=1 且 size 一致；命中后事务插入 t_user_file + ref_count+1；返回 result=1。
- [ ] 未命中：计算 total_chunks（固定 4MB），插入 t_transfer_task(status=0, expire=now+24h)。
- [ ] 失败路径：parent 不存在 1002；改名失败 1003；磁盘/配额 1005；DB 错误 4001~4004。

**gtest 清单:**
- UploadInitCreatesTask
- UploadInitInstantHitCreatesNode
- UploadInitReqIdIdempotent
- UploadInitAutoRename
- UploadInitRejectsBadParent
- UploadInitIgnoresBlobStateNotReady
- UploadInitRedisDownFallsBackToMysql

**验收标准:**
- [ ] 服务层测试全绿；秒传后 f_ref_count 精确 +1。
- [ ] 同一 req_id 调两次返回同一 task_id/uf_id，不产生第二条节点。
- [ ] Redis 不可用时仍能通过 MySQL 完成秒传（fail-open）。

**风险:**
- 两个不同 req_id 并发上传同一文件 → 允许产生两个任务，最终两条用户节点（用户意图）；不要用内容去重锁合并。
- 短锁残留 5s → 锁只影响同一 req，TTL 短；超时返回 1006。

---

### Task U-3：UploadChunk（幂等、乱序、行锁）

**Goal:** 实现 10006：分片落盘、分片 MD5、重复/乱序、done 计数、任务行锁。

**Files:**
- Create: server/logic/upload_chunk_handler.h
- Create: server/logic/upload_chunk_handler.cpp
- Create: server/logic/tests/test_upload_chunk.cpp

**Steps:**
- [ ] 业务线程先 verifyFrameCrc（H2），失败回 9005 + forceClose。
- [ ] 参数校验：task 归属、status∈{0,1}、未过期、index 范围、offset 对齐、data_len 期望。
- [ ] 分片 MD5 校验；写 tmp/<task>/<index>.bin.tmp.<seq> → fsync → rename。
- [ ] 事务：SELECT task FOR UPDATE → 读旧 chunk 状态 → 新增/覆盖/重复三分支 → 只为新完成 +1 → 续期 expire_at → 提交。
- [ ] 重复且 MD5 相同返回 duplicate=1；MD5 不同且已确认返回 3003，不覆盖。
- [ ] 每连接最多 2 个在途分片；超过 pauseRead，ack 后 resume。

**gtest 清单:**
- UploadChunkNewCompletes
- UploadChunkDuplicateNoCount
- UploadChunkDuplicateDifferentMd5Rejected
- UploadChunkOutOfOrder
- UploadChunkOffsetMismatch
- UploadChunkWrongSizeLast
- UploadChunkExpiredTask
- UploadChunkBadCrcCloses
- UploadChunkInFlightLimit

**验收标准:**
- [ ] 乱序上传 13 片成功；done_chunks 最终等于 total。
- [ ] 重复片不改变 done_chunks/tr_offset。
- [ ] 并发同 task 同 index 不产生重复行。
- [ ] 事务内无文件 IO（代码审查）；单事务 <20ms（日志采样）。

**风险:**
- 写盘成功但 DB 失败 → 留下孤儿分片；过期清理按任务目录整删。
- 行锁等待 → 事务短、锁顺序固定；超过 5s 由 MySQL 抛 1205，映射 4004。
- 每连接 2 个在途仍可能塞满业务池 → 业务池队列上限 + 1006/背压。

---

### Task U-4：ContinueUpload 位图

**Goal:** 实现 10028：从 t_chunk 重建位图，修正计数缓存，返回过期时间。

**Files:**
- Modify: server/logic/upload_service.cpp
- Create: server/logic/tests/test_continue_upload.cpp

**Steps:**
- [ ] 校验 task 归属；status 4/5/6 返回对应错误；7 返回 3005。
- [ ] 查 c_state=1 的 c_index，构造 ceil(total/8) 字节位图（LSB-first）。
- [ ] 重算 done_chunks 与 done_bytes；与 tr_done_chunks/tr_offset 不一致时修正并 WARN。
- [ ] 返回 expire_at_ms；位图超过 64KB 时拒绝（total_chunks >524288）。
- [ ] 单测：空任务、部分完成、全完成、计数漂移修正、过期。

**gtest 清单:**
- ContinueUploadEmptyBitmap
- ContinueUploadPartialBitmap
- ContinueUploadCompleteBitmap
- ContinueUploadFixesCounterDrift
- ContinueUploadExpired
- ContinueUploadTaskOfOtherUser

**验收标准:**
- [ ] 13 片任务在第 6 片后位图前 6 位为 1，其余 0。
- [ ] 计数漂移时返回重算值，并把 DB 修正到一致。
- [ ] 不属于当前用户返回 1004，不泄露任务存在性。

**风险:**
- 位图字节序/bit 序错 → 单测固定第 0/7/8 位；客户端用同一实现对照。
- 大任务位图 64KB 上限 → 初始化时校验，不等到续传才报错。

---

### Task U-5：UploadComplete 合并与落库

**Goal:** 实现 10030：选主、逐片校验、整文件 MD5、blob rename、sidecar 生成、事务落库、坏片回传。

**Files:**
- Modify: server/logic/upload_service.cpp
- Create: server/logic/upload_merge_job.h
- Create: server/logic/upload_merge_job.cpp
- Create: server/logic/tests/test_upload_complete.cpp

**Steps:**
- [ ] 先校验 done_chunks==total 且 SUM(c_size)==total_size；缺片回 3002 + 前 32 个缺失下标。
- [ ] 条件 UPDATE status 1→7 选主；失败读状态决定幂等/重试/过期。
- [ ] IO 池合并：1MB 缓冲；逐片 stat+MD5+写 merged；同时更新整文件 MD5 与每 4MB 块 CRC。
- [ ] 坏片：记录 bad_index，删除坏片行/文件，计数回退，status→1，回 3002 + 列表。
- [ ] 整文件 MD5 不符：status→3，返回 3003，清理临时目录。
- [ ] 成功后：fsync merged；用 BlockCrcSidecar::writeFromMerge 写 .crc 临时文件并 fsync；blob 已存在则删 merged，否则 rename；fsync 目录。本任务只实现 sidecar 写入，读取/懒填充在 D-1 扩展。
- [ ] 事务：INSERT t_file ON DUPLICATE +1 → SELECT f_id → 插入 t_user_file（自动改名/复用）→ status=4；提交后回填 Redis。
- [ ] 崩溃窗口：blob 已存在但任务 7；重试时校验 MD5 后跳过 rename，直接落库。

**gtest 清单:**
- UploadCompleteMergesAndCreatesBlob
- UploadCompleteWritesSidecar
- UploadCompleteBadChunkReturnsIndexes
- UploadCompleteWholeFileMd5Mismatch
- UploadCompleteIdempotentDone
- UploadCompleteMergingReturnsRetry
- UploadCompleteBlobExistsSkipsRename
- UploadCompleteAutoRenameAtFinalize
- UploadCompleteCrashAfterRenameRecovers

**验收标准:**
- [ ] 50MB/13 片合并后 blob 大小/MD5 正确；sidecar block_count=13。
- [ ] 坏片重传后只需补坏片即可成功。
- [ ] 同一 task 重复 complete 返回同一 file_id/uf_id，不重复加 ref_count。
- [ ] 合并期间 loop 不被阻塞（IO 池执行；Complete 请求在业务池等待）。

**风险:**
- 合并占满 IO 池 → 全局并发 2；其余排队，超 30s 回 1006。
- rename 与已有 blob 竞争 → 同内容只保留已有 inode；绝不覆盖正在读的文件。
- 99 次改名失败 → 事务回滚并删除本次新建 blob/sidecar；已有 blob 不删。

---

### Task U-6：上传恢复、过期与临时目录清理

**Goal:** 实现服务启动恢复、expiry tick（5min/200）、任务目录清理，并保证只清自己的数据。

**Files:**
- Create: server/app/server_app.h、server/app/server_app.cpp
- Modify: server/app/main.cpp（改为启动 ServerApp）
- Create: server/logic/jobs/expiry_job.h
- Create: server/logic/jobs/expiry_job.cpp
- Create: server/logic/tests/test_upload_expiry.cpp

**Steps:**
- [ ] 启动顺序：DB 池就绪 → UPDATE tr_status=2 WHERE tr_status=7 → 清理 1h 前 merged/tmp → 启动 tick → listen。
- [ ] ExpiryJob::runOnce：SELECT 0/1/2 且 expire_at<NOW LIMIT 200；逐任务条件 UPDATE→6；删 t_chunk 500/批；投 IO 池删目录。
- [ ] 单轮 2s 上限；超时留到下轮，不阻塞 TimerQueue。
- [ ] 测试：过期任务置 6、分片行删除、目录删除；重复 runOnce 幂等；7 不被过期扫描直接清掉。

**gtest 清单:**
- StartupResetsMergingToPaused
- ExpiryMarksOnlyExpired
- ExpiryDeletesChunksAndDir
- ExpiryBatchLimit200
- ExpiryIdempotent
- ExpiryDoesNotTouchActiveTask

**验收标准:**
- [ ] 重启后 status=7 的任务全部变 2，status=1 不变。
- [ ] 过期任务后续 Chunk/Continue 返回 3007。
- [ ] 单轮 200 个任务耗时 <2s（测试库本机）。

**风险:**
- 扫描与上传请求竞争 → 条件 UPDATE 只对 0/1/2 生效；正在写片的请求要么先提交要么被 3007。
- 删目录失败 → 留待孤儿 tmp 清理，不阻断任务置 6。

---

## Stage Download：票据、块路径、限流、续传

### Task D-0：翻转 CM_DOWNLOAD_ZEROCOPY 默认值（门槛任务）

**Goal:** 只有 H1 全绿后，才把零拷贝设为默认；否则保持 OFF 并记录阻塞。

**Files:**
- Modify: cmake/options.cmake
- Modify: docs/benchmark/phase5_zero_copy_gate.md

**Steps:**
- [ ] 前置检查：test_crc32_combine 128 组全绿；记录文件已写。
- [ ] 将 option 默认值改为 ON；保留 -DCM_DOWNLOAD_ZEROCOPY=OFF 可回退。
- [ ] 若任一用例失败：不改默认值，状态标记 Blocked，写明失败 case；下载阶段只交付 OFF 路径。
- [ ] 在 CMake 中确保 OFF/ON 都定义宏，不允许“未定义”分支。

**gtest 清单:**
- 无新增；复用 Crc32CombineRandomized128 作为门槛证据。

**验收标准:**
- [ ] 门槛记录与 ctest 输出一致。
- [ ] OFF 构建与 ON 构建都能编译。
- [ ] 默认值变更只影响块发送实现，不影响协议与测试期望。

**风险:**
- 为了赶进度跳过门槛 → 明确禁止；D-0 是 Blocking gate，不是形式检查。
- 默认 ON 后 sidecar 缺失 → 运行时回退缓冲路径，不是编译错误。

---

### Task D-1：块 CRC sidecar 读取、懒填充与损坏回退

**Goal:** 在 U-5 的写路径之上补 .crc 读取、懒计算与损坏回退。

**Files:**
- Modify: server/logic/block_crc_sidecar.h
- Modify: server/logic/block_crc_sidecar.cpp
- Create: server/logic/tests/test_block_crc_sidecar.cpp

**Steps:**
- [ ] 格式：magic "CMBC1"(5) + version u8(1) + block_size u32 BE + block_count u32 BE + crc[block_count] u32 BE；总长 14+4n。
- [ ] 创建：复用 U-5 的 writeFromMerge（边读边算、临时文件→fsync→rename）；block_size 固定 4194304。
- [ ] 读取：校验 magic/version/block_size/block_count 与 f_size 推导一致；越界/短读返回错误。
- [ ] 懒填充：缺失或 crc=0 的块，IO 池读该块计算 CRC，pwrite 到对应槽位（O_RDWR|O_CREAT）；失败不阻断下载。
- [ ] 损坏：magic/长度不符时视为缺失，重新生成；不信任 sidecar 的 block_count。
- [ ] 单测：正常写读、末块短、crc=0 懒填、截断/错 magic、block_count 不符、并发 pwrite 同一槽位。

**gtest 清单:**
- SidecarWriteReadRoundTrip
- SidecarLastBlockShort
- SidecarLazyFillUnknownCrc
- SidecarCorruptMagicFallsBack
- SidecarBlockCountMismatch
- SidecarConcurrentPwrite

**验收标准:**
- [ ] 13 块 sidecar 大小 14+52=66 字节；每块 CRC 与直接计算一致。
- [ ] 截断/错 magic 时返回错误，不越界读。
- [ ] 懒填充后同块第二次直接命中。

**风险:**
- sidecar 与 blob 不一致 → 上传同遍生成；GC 同删；不一致时重算覆盖。
- 多线程 pwrite 同一槽位 → 同值写，允许；文件打开用 O_RDWR 不截断。

---

### Task D-2：下载票据

**Goal:** 实现 10010 的 ticket 签发、校验、滑动续期与 fail-closed。

**Files:**
- Create: server/logic/download_ticket_cache.h
- Create: server/logic/download_ticket_cache.cpp
- Create: server/logic/tests/test_download_ticket.cpp

**Steps:**
- [ ] 32 字节随机→64 hex；value=uid|fid|hard_exp|flags；SET PX 300000。
- [ ] 每次成功取块 PEXPIRE 300000，但不超过 hard_exp（2h）。
- [ ] 校验 uid/fid/过期；不匹配 1004；Redis 错误 4005/4007，拒绝。
- [ ] 打开时若 uf_id 属于本人且未删，或 share_ticket 有效，签发；目录分享拒绝下载 1001。
- [ ] 单测：正常签发/校验、过期、uid 不符、fid 不符、hard_exp、Redis 不可用拒绝。

**gtest 清单:**
- TicketIssueAndValidate
- TicketExpires
- TicketWrongUser
- TicketWrongFile
- TicketHardExpiry
- TicketRedisDownFailClosed

**验收标准:**
- [ ] 4 个连接共享一张票可并发取块。
- [ ] 超过 5min 未取块后失效；持续取块可滑动到 2h。
- [ ] Redis 不可用时新取块一律拒绝，不 fail-open。

**风险:**
- ticket 泄漏 → 视为 bearer，绑定 uid 校验；日志不打印完整 ticket。
- 2h 硬上限导致超长下载中断 → 重新 10010（start_block=缺失块）换票，不丢进度。

---

### Task D-3：缓冲块发送器（OFF 路径，基线）

**Goal:** 实现不依赖 sendfile 的块读取+CRC+send，作为 OFF 默认与零拷贝的回退。

**Files:**
- Create: server/logic/block_sender.h
- Create: server/logic/block_sender.cpp
- Create: server/logic/tests/test_block_sender_buffered.cpp

**Steps:**
- [ ] 输入：ticket、f_id、block_index；校验 ticket→uid/fid。
- [ ] stat blob 校验大小；算 offset/len（末块短）。
- [ ] IO 池读 4MB（read 循环处理 EINTR/短读），流式 CRC，构造完整帧（19B 头 + payload），回 loop send。
- [ ] 全局并发上限 16；超过排队，不阻塞 loop。
- [ ] 读失败 3001；越界 1001；内存峰值 16×4MB=64MB。
- [ ] 单测：4MB/末块/越界/读失败/16 并发上限。

**gtest 清单:**
- BufferedBlockWhole
- BufferedBlockLastShort
- BufferedBlockOutOfRange
- BufferedBlockReadFailure
- BufferedBlockConcurrencyCap

**验收标准:**
- [ ] 返回帧的 CRC 与直接计算一致。
- [ ] 16 个并发时第 17 个排队而不是 OOM。
- [ ] OFF 构建下 D1~D6 全绿。

**风险:**
- 4MB 内存峰值 → 限制 16；超过排队。
- 读盘阻塞 IO 池 → 业务池不被占用；下载回退优先级低于合并。

---

### Task D-4：sendfile 块发送器（ON 路径）

**Goal:** 用 sidecar CRC + crc32Combine 构造帧头，sendfile 发送 payload，实现零拷贝默认路径。

**Files:**
- Create: server/logic/sendfile_block_sender.h
- Create: server/logic/sendfile_block_sender.cpp
- Modify: server/logic/block_sender.cpp（按 CM_DOWNLOAD_ZEROCOPY 选择）
- Modify: server/CMakeLists.txt（把 sendfile_block_sender.cpp 加入 cm_logic）
- Create: server/logic/tests/test_block_sender_sendfile.cpp

**Steps:**
- [ ] 仅 #if CM_DOWNLOAD_ZEROCOPY 编译；否则返回“unsupported”，由工厂选缓冲路径。
- [ ] 读 sidecar 块 CRC；缺失→懒填充（D-1）或本次回退缓冲路径。
- [ ] 构造 header11（VERSION/TYPE/LEN/SEQ），crc32Combine(crc32(header11), blockCrc, len) 填 CRC。
- [ ] open blob（业务池）；把 fd + prelude 交给 TcpConnection::sendFile；连接接管 fd。
- [ ] header 未发完时 sendFile 必须等 outputBuffer 空；每轮 ≤1MB；EAGAIN 继续。
- [ ] 单测：4MB 帧与缓冲路径逐字节一致；末块；sidecar 缺失回退；fd 关闭；prelude 顺序。

**gtest 清单:**
- SendfileBlockMatchesBuffered
- SendfileBlockLastShort
- SendfileMissingSidecarFallsBack
- SendfileHeaderBeforePayload
- SendfileFdClosed
- BlockSenderFactorySelectsBufferedWhenDisabled（OFF 构建，工厂测试）

**验收标准:**
- [ ] ON 构建下 4MB 块字节/CRC 与 OFF 一致。
- [ ] 零拷贝路径用户态不分配 4MB payload（代码审查 + 峰值 RSS 对比记录）。
- [ ] sidecar 损坏/缺失不影响下载成功，只回退变慢。

**风险:**
- 帧头 CRC 合成错误 → H1 对拍 + D-4 与缓冲路径逐字节对比双重保证。
- sendfile 磁盘读阻塞 loop → 监控 loop 延迟；P99>5ms 时该连接切缓冲路径。
- prelude 与 payload 顺序错 → TcpConnection 单测 + 本任务集成测试。

---

### Task D-5：令牌桶、并发上限、慢客户端

**Goal:** 实现每连接/每用户限速、全局 512 流上限、30s 无进展关闭。

**Files:**
- Create: server/logic/rate_limiter.h
- Create: server/logic/rate_limiter.cpp
- Create: server/logic/download_limits.h
- Create: server/logic/tests/test_rate_limiter.cpp

**Steps:**
- [ ] 令牌桶：容量 4MB，速率 2MB/s（连接）；用户容量 16MB，8MB/s；steady_clock 补充令牌。
- [ ] 令牌不足：计算等待 ms，TimerQueue::runAfter 重试；等待 >2s 回 3006。
- [ ] 全局活跃流计数 512、每用户 8；超限 3006；连接关闭/完成归还。
- [ ] 慢客户端：sendfile 无字节进展 30s → forceClose；每连接只保留 1 个在途块。
- [ ] 单测：桶容量/补充、等待不阻塞线程、用户上限、全局上限、慢客户端超时（注入 fake clock）。

**gtest 清单:**
- TokenBucketRefill
- TokenBucketWaitCalculated
- TokenBucketRateZeroReturns3006
- UserStreamLimit8
- GlobalStreamLimit512
- SlowClientNoProgressClosed
- StreamSlotReleasedOnClose

**验收标准:**
- [ ] 限速下 50MB 下载耗时符合速率（允许 ±20%）。
- [ ] 限速等待期间 loop 可处理其他连接（测试用 1ms tick 观察）。
- [ ] 连接关闭后槽位归零，重复 600 次不泄漏计数。

**风险:**
- 用户级 map 锁竞争 → 按 uid 分段/短临界区；Phase 7 压测再优化。
- Timer 风暴 → 每个等待只注册一个定时器；令牌足够时取消。
- 慢客户端判定误杀 → 只在“有在途块且 30s 零进展”时关闭。

---

### Task D-6：DownloadService 与 10010/10027/10013/10040

**Goal:** 串起打开、续传校验、取块、完成上报，并保证两条块路径行为一致。

**Files:**
- Create: server/logic/download_service.h
- Create: server/logic/download_service.cpp
- Create: server/logic/download_handlers.h
- Create: server/logic/download_handlers.cpp
- Create: server/logic/tests/test_download_service.cpp

**Steps:**
- [ ] 10010：鉴权→uf_id/share_ticket→t_file→stat→ticket；返回 size/md5/block_size/block_count。
- [ ] 10027：返回远端元信息，供客户端校验 .part。
- [ ] 10013：ticket 校验→block 范围→BlockSender（编译开关选择）→发送。
- [ ] 10040：校验 f_id/size/md5；verify_ok=0 记 WARN/指标；Redis 去重 10min；无 DB 写入。
- [ ] 错误映射：1004/1001/3001/3006/4005/4007/4008。
- [ ] 单测：越权、过期、越界、末块、完成上报幂等、Redis 挂拒绝。

**gtest 清单:**
- DownloadOpenOwnFile
- DownloadOpenRejectsOtherUser
- DownloadOpenMissingBlob
- DownloadStatMismatchDetected
- DownloadRangeOutOfBounds
- DownloadRangeTicketExpired
- DownloadDoneIdempotent
- DownloadDoneMd5MismatchWarns

**验收标准:**
- [ ] OFF/ON 两套构建的 D1~D6 全绿。
- [ ] 4 个连接并发下载 50MB，MD5 一致。
- [ ] 票据/限流依赖 Redis 不可用时按表拒绝，不静默放行。

**风险:**
- 下载服务误在 loop 线程做 stat/open → 全部投业务池；代码审查。
- 10011 无 code 字段 → 失败只能走 ERROR_RS；客户端要能处理。
- 完成上报无 DB 写入 → 指标丢失可接受，不影响正确性。

---

## Stage Share / Recycle / List

### Task S-1：ShareService

**Goal:** 实现 6 位分享码、1/7/永久、4 位提取码、缓存与提取码锁定。

**Files:**
- Create: server/logic/share_service.h
- Create: server/logic/share_service.cpp
- Create: server/logic/tests/test_share_service.cpp

**Steps:**
- [ ] 码生成：31 字符表 + 拒绝采样；INSERT 1062 重试 5 次；统一大写。
- [ ] 有效期：0→9999 哨兵、1→+1d、2→+7d；wire 0 表示永久。
- [ ] 提取码：1000~9999；5 次/10min 锁定；Redis 不可用 fail-closed 4008。
- [ ] 缓存：share/{code} 5min±10%；miss 60s；取消/改名/删除失效。
- [ ] req_id 幂等 10min；Redis 不可用允许重复创建但不损坏数据。
- [ ] 测试：字符表、碰撞重试、提取码、锁定、过期、取消、引用计数不变。

**gtest 清单:**
- ShareCodeAlphabetOnly
- ShareCodeCollisionRetry
- ShareCreateIdempotent
- ShareOpenNoAccessCode
- ShareOpenWrongCodeLocks
- ShareExpired
- ShareCancelIdempotent
- ShareAccessDoesNotChangeRefCount

**验收标准:**
- [ ] 1000 次生成无易混字符；重复码重试成功。
- [ ] 第 6 次错误提取码起锁定 10min；Redis 挂返回 4008。
- [ ] 访问分享前后 f_ref_count 不变。

**风险:**
- 枚举 6 位码 → IP 30 次/分钟限流；分享码不是秘密，但不能无限试。
- 缓存显示名过期 → 改名/移动必须 DEL share key。

---

### Task S-2：RecycleService

**Goal:** 实现软删、恢复、彻底删除、目录子树与计数轨迹。

**Files:**
- Create: server/logic/recycle_service.h
- Create: server/logic/recycle_service.cpp
- Create: server/logic/tests/test_recycle_service.cpp

**Steps:**
- [ ] 删除：事务锁节点→已删短路→置 deleted_at→文件减计数→0 置孤儿→失效分享缓存。
- [ ] 恢复：父节点缺失/已删→根；名称冲突自动改名；已恢复幂等返回。
- [ ] 彻底删除：文件只删行；目录 BFS 子树，>10000 返回 3008，200 行/事务，活跃文件补减计数，已软删不二次减。
- [ ] 每批取消指向该子树的分享。
- [ ] 测试：删除/恢复/彻底删除计数轨迹；目录子树；超限；重复调用幂等。

**gtest 清单:**
- DeleteDecrementsOnce
- RestoreIncrementsOnce
- PurgeFileNoDoubleDecrement
- PurgeTreeActiveDescendants
- PurgeTreeTooLarge
- PurgeIdempotent
- RecycleListCursorStable

**验收标准:**
- [ ] 删除→恢复→再删除→彻底删除后 ref_count 与节点状态一致。
- [ ] 10001 节点目录返回 3008，不部分删除。
- [ ] 彻底删除后回收站列表不再出现。

**风险:**
- 目录子树长事务 → 200 行/事务、根最后删、可重跑。
- 计数下溢 → 条件 >0 + 前置校验；测试覆盖。

---

### Task S-3：FileService 列表分页

**Goal:** 实现 10008：三种排序的 keyset 分页、total_count 上限、非法 cursor 拒绝。

**Files:**
- Create: server/logic/file_service.h
- Create: server/logic/file_service.cpp
- Create: server/logic/tests/test_file_list.cpp

**Steps:**
- [ ] sort=0：(uf_name, uf_id) > (?,?)；sort=1：(created_at,id)<；sort=2：(updated_at,id)<。
- [ ] limit 1~200，默认 50；取 n+1 判 has_more。
- [ ] 首页 COUNT 上限 10000，200ms max_execution_time；超时返回未知。
- [ ] 非法 cursor 长度/排序字段返回 1001。
- [ ] 测试：1000 行翻页、同微秒、并发插入、limit 越界。

**gtest 清单:**
- FileListNameAscendingStable
- FileListCreatedDescSameMicrosecond
- FileListUpdatedDesc
- FileListHasMore
- FileListInvalidCursor
- FileListTotalCountCap
- FileListNoOffsetSql

**验收标准:**
- [ ] 1000 行每页 50 翻完无重复/漏行。
- [ ] 同微秒批量插入仍稳定。
- [ ] SQL 不含 OFFSET。

**风险:**
- 排序字段与索引不一致 → 逐条 EXPLAIN 记录（人工），保证 key 前缀。
- COUNT 慢 → 首页才查、超时降级。

---

### Task S-4：PlayService

**Goal:** 实现 10032 播放进度 upsert 与 LWW。

**Files:**
- Create: server/logic/play_service.h
- Create: server/logic/play_service.cpp
- Create: server/logic/tests/test_play_service.cpp

**Steps:**
- [ ] 校验 f_id 存在且属于当前用户可见（列表节点存在）。
- [ ] position<=duration（duration>0 时）；非法 1001。
- [ ] upsert；重复上报同值不产生第二行。
- [ ] 测试：首次写、更新、回退、finished、非法位置。

**gtest 清单:**
- PlayCreate
- PlayUpdateLww
- PlayRewindAllowed
- PlayInvalidPosition
- PlayOneRow

**验收标准:**
- [ ] 同一 (uid,fid) 永远一行。
- [ ] 回退进度被接受（LWW）。
- [ ] 无 Redis 依赖。

**风险:**
- 客户端高频上报 → 客户端 10s 节流；服务端不做额外缓存。

---

## Stage Timers：过期、GC、分享扫描

### Task T-1：ExpiryJob 接入 ServerApp

**Goal:** 把上传过期任务挂到 TimerQueue，启动/退出顺序不变。

**Files:**
- Modify: server/app/server_app.cpp
- Modify: server/app/server_app.h
- Create: server/logic/jobs/expiry_job.h/.cpp（若 U-6 未创建）
- Create: server/logic/tests/test_expiry_job.cpp

**Steps:**
- [ ] ServerApp 在 listen 前执行恢复 SQL；listen 后注册 runEvery(5min)。
- [ ] 回调只做“取 ID + 投业务池”，不在 loop 里跑 SQL/IO。
- [ ] 退出时取消定时器，等待在途任务结束。
- [ ] 测试：runOnce 可独立调用；批量 200；幂等；7 不被清。

**gtest 清单:**
- ExpiryJobRunOnce
- ExpiryJobBatch200
- ExpiryJobIdempotent
- ExpiryJobSkipsMerging
- ExpiryJobDoesNotBlockLoop（回调 <1ms，投递计数）

**验收标准:**
- [ ] 启动后 5min 内触发一次；退出 <5s。
- [ ] loop 回调不执行 SQL（代码审查/计时）。

**风险:**
- 定时器回调里误做重活 → 只投递；超 1ms 记 WARN。

---

### Task T-2：Orphan GC、分享过期、播放记录清理

**Goal:** 实现每日 GC 与分享过期扫描，安全回收 blob/.crc。

**Files:**
- Create: server/logic/jobs/orphan_gc_job.h/.cpp
- Create: server/logic/jobs/share_expiry_job.h/.cpp
- Create: server/logic/jobs/play_cleanup_job.h/.cpp
- Create: server/logic/tests/test_gc_job.cpp

**Steps:**
- [ ] GC：24h 宽限 + f_state=2 + 无活跃节点 + 无有效分享；LIMIT 100；单轮 2GB。
- [ ] 逐个事务 re-check f_ref_count=0；IO 池 unlink blob 与 .crc；成功后删 t_file；失败保留并告警。
- [ ] 分享过期：每日 03:10，1000 行/轮，置 s_status=1。
- [ ] 播放清理：每月 1 日，删无 f_id 的孤儿记录 500 行/轮。
- [ ] 测试：宽限内不删、分享中不删、ref=0 删除、unlink 失败保留、批量上限。

**gtest 清单:**
- GcSkipsWithinGrace
- GcSkipsActiveNode
- GcSkipsActiveShare
- GcDeletesBlobAndSidecar
- GcUnlinkFailureKeepsRow
- GcBatchLimit100
- ShareExpirySweep
- PlayCleanupOrphans

**验收标准:**
- [ ] 25h 前孤儿被删；1h 前不动。
- [ ] 有效分享指向的 blob 不被删。
- [ ] unlink 失败行保留，下轮重试。

**风险:**
- 误删 → 三重 re-check + 24h；只删 f_ref_count=0。
- 大规模 unlink 拖垮磁盘 → 100 行/2GB 上限。

---

### Task T-3：定时任务总验收

**Goal:** 在真实 TimerQueue + DB 环境下验证批量、时序、退出。

**Files:**
- Create: server/logic/tests/test_timers_integration.cpp

**Steps:**
- [ ] 启动 ServerApp，注入 250 个过期任务，等待 tick，验证只处理 200、下一轮 50。
- [ ] 注入 150 个孤儿，验证单轮 100。
- [ ] SIGINT 退出，验证定时器取消、无 use-after-free。
- [ ] ASan/LSan 跑一遍。

**gtest 清单:**
- TimerExpiryTwoRounds
- TimerGcBatch
- TimerShutdownNoLeak

**验收标准:**
- [ ] 两轮处理完 250；无重复处理。
- [ ] 退出 <5s；LSan 干净。

**风险:**
- 测试等待 5min 太久 → 用可注入的 tick 间隔（1s）只用于测试；生产仍 5min。

---

## Stage E2E：CLI、杀进程、降级演练

### Task E-1：upload_cli

**Goal:** 交付可脚本化的上传 CLI，支持 -c/-r/--md5/--stop-after-chunks。

**Files:**
- Create: server/examples/upload_cli.cpp
- Modify: server/CMakeLists.txt

**Steps:**
- [ ] 参数解析：host/port/user/password/file/remote/parent-id/-c/-r/--md5/--state/--stop-after-chunks/--keep-task。
- [ ] 登录→10004（或 -r 读 state 发 10028）→逐片 10006→10030。
- [ ] state 文件写 task_id、已发片、本地 MD5；先写临时文件再 rename。
- [ ] --stop-after-chunks N 调用 _exit(9)，不取消任务。
- [ ] 输出 KEY=VALUE；退出码 0/2/3/4/9。
- [ ] 密码只从参数/环境读，不打印。

**gtest 清单:**
- 无 gtest；由 E-3 的端到端用例覆盖。
- 静态检查：无密码日志、无裸 new/delete。

**验收标准:**
- [ ] 50MB 文件一次上传成功，服务端 MD5 一致。
- [ ] --stop-after-chunks 6 后 DB 中任务仍在且 done_chunks=6。
- [ ] -r 只发剩余片。

**风险:**
- CLI 与协议字段漂移 → 直接复用 common/protocol/messages，不手写字节。
- _exit(9) 不 flush → state 必须在每片 ack 后落盘。

---

### Task E-2：download_cli

**Goal:** 交付 4 连接并发、块位图续传、MD5 校验的下载 CLI。

**Files:**
- Create: server/examples/download_cli.cpp
- Modify: server/CMakeLists.txt

**Steps:**
- [ ] 参数：--uf-id/--share-code/--access-code/--out/-c/-j/-r/--md5/--stop-after-blocks/--rate。
- [ ] 10010 打开→10027 校验→按块位图分配 -j 个连接→10013 取块→seek+write。
- [ ] .part 与 .part.state（size/md5/block_size/bitmap）；先写临时再 rename。
- [ ] 每块 Codec 校验帧 CRC；完成后整文件 MD5；发 10040。
- [ ] --stop-after-blocks N → _exit(9)；-r 读 state 只取缺失块。

**gtest 清单:**
- 无 gtest；由 E-3 覆盖。
- 静态检查：4 连接共享 ticket；无 4MB 常驻（CLI 侧可接受单块缓冲）。

**验收标准:**
- [ ] 50MB 下载 MD5=25e317773f308e446cc84c503a6d1f85。
- [ ] 杀进程后 -r 只取剩余块。
- [ ] --rate 256KB/s 下服务端 loop 不阻塞（观察其他接口延迟）。

**风险:**
- 多连接写同一文件偏移 → 每块独立 seek+write，互不重叠。
- state 半写 → 临时文件+rename。

---

### Task E-3：VM 杀进程与续传验收

**Goal:** 用 VM 上的 50MB 文件跑通 U4/U5/D2 与服务端重启场景。

**Files:**
- Create: scripts/e2e_phase5.sh（只做编排，不含密码）
- Create: docs/benchmark/phase5_e2e.md

**Steps:**
- [ ] 校验文件：md5sum ~/cm_testdata/test_50m.bin = 25e317773f308e446cc84c503a6d1f85。
- [ ] 上传：--stop-after-chunks 6 → 查 DB → -r → 比对 MD5。
- [ ] 服务端重启：第 6 片后 systemctl restart → -r → 成功。
- [ ] 下载：--stop-after-blocks 6 → -r → 比对 MD5。
- [ ] 记录耗时、恢复位图、失败码；不记录密码。

**gtest 清单:**
- 无；这是人工/脚本验收，覆盖 U4/U5/D2。

**验收标准:**
- [ ] 三条链路都通过；MD5 一致。
- [ ] 服务端日志能看到 7→2 恢复与续传请求。
- [ ] 报告写入 docs/benchmark/phase5_e2e.md。

**风险:**
- VM 磁盘/内存不足 → 先 df/free 检查；50MB 文件足够。
- 系统服务方式与手工启动路径不同 → 两种都记录。

---

### Task E-4：缓存降级演练

**Goal:** 验证 Redis/MySQL 故障时的 fail-open/fail-closed 行为与错误码。

**Files:**
- Create: scripts/degrade_drill.sh
- Create: docs/benchmark/phase5_degrade.md

**Steps:**
- [ ] 停 Redis：秒传应回源 MySQL 成功；分享无提取码可访问；分享带提取码返回 4008；下载打开返回 4005/4007；已登录请求返回 4007。
- [ ] 停 MySQL：所有读写返回 4001/4002，不返回空列表/假成功。
- [ ] 恢复 Redis/MySQL 后 5s 内自愈（连接池熔断半开）。
- [ ] 记录每个接口的 code、耗时、客户端表现；不记录密码。

**gtest 清单:**
- 无 gtest；人工演练。可补 DegradeMappingTest 纯函数测试（错误码映射）。

**验收标准:**
- [ ] 降级表逐项符合 06 第 10 节。
- [ ] 鉴权/票据/限流在 Redis 挂时拒绝，不 fail-open。
- [ ] 恢复后无需重启服务。

**风险:**
- 停 Redis 影响其他测试 → 在独立环境/独立 db 做；演练脚本先确认环境变量。
- 熔断 5s 导致恢复延迟 → 记录并接受。

---

### Task E-5：文档回填与阶段收尾

**Goal:** 把实现与 02/05/06 的差异回写，保证下一阶段不再猜。

**Files:**
- Modify: docs/specs/02-协议设计.md（差异清单）
- Modify: docs/specs/05-存储设计.md（tr_offset 语义、分享码、GC）
- Modify: docs/specs/06-业务设计.md（状态改 Done、门槛结果链接）
- Modify: README / CODEBUDDY.md（构建选项、CLI 用法，如适用）

**Steps:**
- [ ] 02：10011/10024 复用、10030~10043、业务 RS code、下载数据纯 raw、分享码 6 位。
- [ ] 05：tr_offset 改为“已完成字节总和”、GC 自动回收、s_code CHAR(6)。
- [ ] 06：补 H1/H2 实际测试结果与开关默认值。
- [ ] CODEBUDDY.md：CM_DOWNLOAD_ZEROCOPY、upload_cli/download_cli 用法。
- [ ] 跑一次依赖红线 grep（Qt/FFmpeg/server include）。

**gtest 清单:**
- 无；静态检查与文档 review。

**验收标准:**
- [ ] 文档与代码字段/类型号/默认值一致。
- [ ] 新机器按 README 能构建；CLI --help 可用。
- [ ] 无密码/token 进入文档。

**风险:**
- 文档再次漂移 → 把类型号与常量表放在 06 附录 B，改动必须同步。

---

## 5. Verification（阶段总验收）

**测试注册约定：** 新增测试一律用 add_test(NAME <短名> COMMAND <exe> ...) 显式注册，短名与本文命令一致；P0 至少注册 md5、crc32_combine、bulk_gate、send_file、cache_key、schema_v2_2，后续 Stage 至少注册 upload、download、share、recycle、list、timers、degrade。

- [ ] P0：ctest -R 'md5|crc32_combine|cache_key|bulk|codec|buffer|send_file' 全绿。
- [ ] H1 门槛：128 组 crc32Combine 对拍全绿；否则 CM_DOWNLOAD_ZEROCOPY 保持 OFF。
- [ ] H2 门槛：bulk_gate 7 例全绿；连续 10 个 4MB 帧后无 4MB 常驻；ASan/LSan 干净。
- [ ] DAO：无 DB 单测全绿；有 DB 集成全绿；引用计数轨迹正确。
- [ ] 上传：U1~U11 对应服务层/CLI 用例全绿；杀进程续传成功。
- [ ] 下载：OFF 与 ON 两套构建的 D1~D6 全绿；50MB MD5 一致。
- [ ] 分享/回收站/列表：S1~S5、R1~R4、P1~P2 全绿。
- [ ] 定时任务：5min/200、每日/100、24h 宽限、幂等、退出 <5s。
- [ ] 降级：Redis/MySQL 故障行为符合 06 第 10 节。
- [ ] 代码：-Wall -Wextra 零警告；无裸 new/delete 管资源；无 C++14/17；无 OFFSET 大分页；无密码日志。

**OFF/ON 双构建矩阵（人工执行）：**

~~~bash
# OFF
mkdir -p build-off && cd build-off
cmake .. -DBUILD_SERVER=ON -DBUILD_STORAGE=ON -DBUILD_TESTS=ON -DCM_DOWNLOAD_ZEROCOPY=OFF
make -j8
ctest -R 'download|block_sender|crc32_combine' --output-on-failure

# ON（仅在 H1 全绿后）
mkdir -p build-on && cd build-on
cmake .. -DBUILD_SERVER=ON -DBUILD_STORAGE=ON -DBUILD_TESTS=ON -DCM_DOWNLOAD_ZEROCOPY=ON
make -j8
ctest -R 'download|block_sender|crc32_combine' --output-on-failure
~~~

---

## 6. Rollback

- **零拷贝回退**：重新配置 -DCM_DOWNLOAD_ZEROCOPY=OFF 并构建；无需改协议/数据，sidecar 保留不影响 OFF 路径。
- **6 位分享码回退**：需同时回退 DDL、CacheKey 与服务层；已产生的 6 位分享码无法被 9 位逻辑读取，属于不兼容回退，必须在低峰执行并清理/重建分享数据。
- **CodecBulk 回退**：保留原 decode；ConnectionDecoder 可切回“全走 decode”（小帧模式），但 4MB CRC 会回到 IO 线程，仅作为临时排障，不作为生产默认。
- **合并失败**：任务保持 7/1，blob 未落库；重启把 7 改 2，客户端可重试；临时目录由 expiry/tmp 清理。
- **GC 误删**：24h 宽限 + 对账；一旦 unlink 不可恢复，只能从备份恢复，故 GC 必须先 dry-run 一轮并核对计数。

---

## 7. Blocking Questions

无。06 的 4 项冲突已裁决；若评审推翻分享码 6 位或 10011/10024 复用，P0-3/P0-4 与消息表需重做，届时升级为 Blocking 变更。

---

## 8. Deferred Decisions

- **tr_offset 改名 tr_done_bytes**：语义更准，但需改 05/DAO/协议字段名；当前保留 tr_offset，注释明确“已完成字节总和”。
- **下载限速默认值**：2MB/s/连接、8MB/s/用户为初值；Phase 7 压测后调整，不改接口。
- **单用户配额**：本期只做磁盘 NoSpace；配额表/字段留到后续。
- **目录分享展开下载**：本期只展示元信息；需要子树授权与打包，留到后续。
- **懒 sidecar 生成的并发写**：同槽位同值 pwrite 允许；若 Phase 7 发现 IO 抖动，再加每 blob 锁。
- **CM_DOWNLOAD_ZEROCOPY 的运行时开关**：本期只做编译开关；运行时不动态切换，避免两套路径状态不一致。
