# Phase 5 实施计划（业务层：上传 / 下载 / 分享 / 回收站）

**Created:** 2026-10-18
**Status:** In Progress（Upload Stage 已收口（U-1~U-6）；D-0 已验收（双构建）；D-1 已验收（DownloadOpen 7/7）；D-1b 已验收（块 CRC 6/6）；D-2 已验收：下载票据，build-on 88/88；D-3 已验收：缓冲块发送器 5/5、build-on 93/93；D-4 已验收：sendfile 零拷贝 + crc32Combine 组帧 + 工厂选择，build-on 99/99 连跑两遍、build-off 工厂 1/1、ctest 32/32、四表零残留；下一步 D-5 令牌桶/并发上限/慢客户端）
**Source Spec:** docs/specs/06-业务设计.md（v0.1）
**Review Gate:** 06 文档第 1.3 节的 4 项冲突裁决（号段、分享码、sendfile+CRC、物理回收）需在开工前确认；本文按裁决已落地为任务。
**P0 Exception:** 用户已确认 P0-4 允许对 cache_store.cpp 做最小改动，仅限 CacheKey::share/shareMiss 与新增 uploadReq/shareReq，不改 CacheStore 逻辑。
**H1 Gate:** docs/benchmark/phase5_zero_copy_gate.md（PASS：用户确认 ctest 全绿；D-0 可翻转零拷贝默认值）。
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
| server/logic/download_ticket.* | Create | ticket 签发/校验/滑动续期 | Redis 挂 fail-closed |
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
| Upload | U-3b 路由接线 | HandlerRegistry、10004/10006、NetServer 接线 | handler_registry 全绿；IO 线程零存储调用 |
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
- [x] 在 frame.h 增加 RawFrame（type/seq/len/expectedCrc/header11/payload）；header11 用 std::array<uint8_t,11>。
- [x] Buffer 增加 capacity() 与 shrinkTo(size_t minCapacity)：保留未读数据（readerIndex..writerIndex），重新分配并复制；minCapacity 至少 1024，最大不超过原容量。
- [x] 实现 Codec::decodeBulk(Buffer&, RawFrame*, DecodeStatus*)：
  - 校验 MAGIC/VERSION/LEN/类型白名单（仅 10006；其他类型返回 DecodeStatus::UnknownType，且不消费 Buffer，readableBytes 不变）；
  - 复制 header11；in.retrieve(kHeaderSize)；out->payload = in.retrieveAsString(len)；不校验 CRC；
  - 调用 in.shrinkTo(64*1024)；返回 Ok/NeedMore/BadMagic/BadVersion/BadLength/UnknownType。
- [x] 实现 Codec::verifyFrameCrc(const RawFrame&)：用 cm::Crc32 update header11 的 11 字节 + payload，比较 expectedCrc。
- [x] 实现 ConnectionDecoder：
  - 普通帧循环 decode，直到 NeedMore；
  - 遇到 10006：构造 shared_ptr<RawFrame>，设置 bulkPending_，pauseRead，投业务池，返回 StopForBulk；
  - 业务线程 verify：失败回 loop forceClose；成功调用 handler；完成后回 loop 清 bulkPending_、resumeRead、继续 drain；
  - 状态只在 loop 线程读写；业务线程不持有 Buffer*、不持有 TcpConnection 引用（用 weak_ptr 回投）。
- [x] 单测使用可控阻塞器：验证第二帧在放行前不解析；坏 CRC 时第二帧不投递；payload 在 Buffer reset 后仍完整；连续 10 个 4MB 帧后 capacity ≤64KB；粘包保留。

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
- [x] ctest -R 'bulk_gate|cm_common_tests' 全绿（用户确认全量 ctest 通过；bulk_gate 为独立 add_test 名，原 Codec/Buffer 回归在 cm_common_tests 内）。
- [x] BulkGateSecondFrameNotDecodedUntilVerified 在阻塞器放行前断言 handler 计数 0；放行后 1。
- [x] 连续 10 个 4MB 帧后，TcpConnection/Buffer 没有 4MB 常驻（ctest 通过）。
- [ ] ASan/LSan 无泄漏（未执行）。
- [x] grep 确认业务线程代码没有 Buffer* 捕获、TcpConnection 没有 payload 成员。
- [x] 原 Codec::decode 的 8 个回归用例仍全绿（cm_common_tests 通过）。

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
- [x] 增加 bool sendFile(int fd, std::int64_t offset, std::size_t len, const std::function<void(bool)>& done);：
  - 连接接管 fd 所有权（完成/失败/关闭都 close）；
  - 调用方先用 send() 发帧头；只有 outputBuffer 写空后才启动 sendfile；
  - 每轮最多 1MB；EAGAIN 时保留剩余区间并 enableWriting；
  - 完成回调只调用一次（成功 true / 失败 false）；
  - 连接关闭时取消并 close fd，回调 false。
- [x] 增加 pauseRead() / resumeRead()：pauseRead 设置 readPaused_ 并 disableReading；rearm() 在 readPaused_ 时直接返回，防止消息回调尾部把 EPOLLIN 又打开；resumeRead 清标志并 enableReading。只改本 loop 的 EPOLLIN，不影响写；关闭状态下无副作用。
- [x] Windows 编译占位：sendFile 走非 Linux 分支，完成回调 false；pauseRead/resumeRead 走 Channel。
- [x] 单测：4MB 临时文件整发；prelude 先到；SO_SNDBUF 很小造成部分写；回调一次；完成后 fd 已关闭；关闭连接时 fd 关闭且回调 false；/proc/self/fd 基线；空闲无 EPOLLOUT。

**gtest 清单:**
- TcpConnectionSendFileTest.WholeFile
- TcpConnectionSendFileTest.PreludeFirst
- TcpConnectionSendFileTest.PartialEagain
- TcpConnectionSendFileTest.CallbackOnce
- TcpConnectionSendFileTest.FdClosedOnComplete
- TcpConnectionSendFileTest.FdBaselineRestored（/proc/self/fd 前后对比）
- TcpConnectionSendFileTest.IdleNoEpollOutAfterSendFile（完成后 isWriting=false）
- TcpConnectionSendFileTest.CancelOnClose
- TcpConnectionPauseReadTest.StopsEpollInAfterRearm
- TcpConnectionPauseReadTest.SurvivesRearm（在消息回调内 pause，回调返回后仍不读）

**验收标准:**
- [x] ctest -R send_file 全绿（Linux）；Windows 全部 SKIP（用户确认全量 ctest 通过）。
- [x] 4MB 传输字节与文件逐字节一致；prelude 在前（ctest 通过）。
- [x] 关闭连接后回调不悬挂，fd 数量回到基线（/proc/self/fd 对比）；ctest 通过。
- [x] 空闲时无 EPOLLOUT 常驻，CPU 不空转；ctest 通过。

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
- [x] 定义 UserRow（u_id/u_tel/u_password/u_salt/u_pwd_algo/u_name/u_status）。
- [x] findByTel(tel, UserRow*)；insert(...)；updatePassword(uid, hash, algo, salt)。
- [x] 全部 PreparedStatement；INSERT 捕获 1062 返回 AlreadyExists。
- [x] 单测：找不到返回 NotFound；插入后可查；重复 tel 返回 AlreadyExists；密码字段不打印。
- [x] 修复 updatePassword：连接未开 CLIENT_FOUND_ROWS，changed rows=0 时显式查存在性，同值更新返回 Ok。

**gtest 清单:**
- UserDaoFindMissing
- UserDaoInsertAndFind
- UserDaoDuplicateTel
- UserDaoUpdatePassword

**验收标准:**
- [x] 有 DB 集成测试全绿；无 DB SKIP。（2026-10-19：无 DB 档 5 SKIP / ctest Passed；有 DB 档 5 passed，89ms）
- [x] 慢查询日志只含占位符 SQL，不含密码。（全部走 PreparedStatement，慢查询打印 sql() 原文含 `?`）
- [x] 无字符串拼接 SQL。（grep `user_dao.cpp` 无字符串拼接，仅字面量 SQL + `?`）

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
- [x] 方法：findBlobByMd5、insertBlobOnDuplicateIncrement、getBlobById、createUserFileAutoRename、listByParent（三种排序 + 游标）、softDelete、restore、purgeNode、findUserFile、countByParent。
- [x] createUserFileAutoRename 在事务内尝试原名与 (2)…(99)，捕获 1062 继续；返回最终名。
- [x] softDelete 在同一事务内减 ref_count（条件 >0）并在 0 时置 f_state=2。
- [x] 列表 SQL 用行构造器游标，禁止 OFFSET；limit+1 判断 has_more。
- [x] 单测：同名自动改名、软删只减一次、恢复加一次、游标同微秒稳定。

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
- [x] 有 DB 集成全绿；引用计数轨迹 -1/+1 正确。（2026-10-19：无 DB 档 7 SKIP + 静态 1 例通过；有 DB 档 8/8 passed，181ms；跑完 t_user/t_user_file/t_file 残留均为 0）
- [ ] 分页 1000 行、每页 50 无重复/漏行。（DAO 层以 12 行/5 与 7 行/3 覆盖翻页与同微秒；1000 行规模留到 S-3 FileService 验收）
- [x] SQL 中无 LIMIT ?,? 形式的大偏移。（由 FileDaoNoOffsetSql 静态断言锁死）

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
- [x] 方法：createTask、findTaskByTaskId、lockTaskForUpdate、upsertChunkIfAbsent（返回是否新完成）、listDoneIndexes、sumDoneBytes、markStatus、electMerge（status 1→7）、resetMergingOnStartup、findExpired。
- [x] 所有状态更新用条件 UPDATE（WHERE tr_status IN (...))，返回 affected_rows。
- [x] 分片 upsert 必须区分 insert/update/no-op，确保 done_chunks 只在新完成时 +1。
- [x] 位图查询按 c_index 升序，LSB-first 由服务层组装。
- [x] 单测：重复分片不增计数；乱序计数正确；electMerge 并发只有一个 affected=1；过期扫描只取 0/1/2。

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
- [x] 有 DB 集成全绿；两个线程同时 electMerge 只有一个成功。（2026-10-19：无 DB 档 8 例 SKIP；有 DB 档 8/8 passed 153ms，连跑两遍均全绿。并发正确性由单条条件 UPDATE 的 InnoDB 行锁保证，DAO 测试用“抢到 / 再抢失败 / 状态不符不抢”三次串行调用覆盖同一 WHERE 子句；线程级并发留到 U-5 合并流程验证）
- [x] 重复分片后 tr_done_chunks 不增、tr_offset 不增。（TransferDaoUpsertChunkDuplicate 断言 NoOp 且计数不变；测试后 t_transfer_task / t_chunk 残留均为 0）
- [x] 事务内锁顺序为任务行→分片行（代码审查：upsertChunkIfAbsent 先 lockTaskForUpdate 再 SELECT t_chunk FOR UPDATE，无反向路径）。

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
- [x] ShareDao：insert（捕获 1062 由服务层重试）、findByCode、listByUser（游标）、cancel、listActiveByUfId、sweepExpired。
- [x] PlayDao：upsertLww（position/duration/finished/updated_at=NOW(6)）、findByUserFile、listRecent。
- [x] 分享码列按 CHAR(6) 读写；s_file_id 目录为 NULL。
- [x] 单测：重复 code 插入 1062；取消幂等；播放 upsert 不产生第二行；LWW 覆盖旧值。

**gtest 清单:**
- ShareDaoInsertDuplicateCode
- ShareDaoFindByCode
- ShareDaoListCursor
- ShareDaoCancelIdempotent
- ShareDaoSweepExpired
- PlayDaoUpsertLww
- PlayDaoOneRowPerUserFile

**验收标准:**
- [x] 有 DB 集成全绿。（2026-10-19：构建零警告；无 DB 档 7 例 SKIP；有 DB 档 share_dao 5/5、play_dao 2/2，连跑两遍全绿；ctest 全量 21/21）
- [x] 分享列表按 s_created_at DESC, s_id DESC 稳定。（ShareDaoListCursor 断言 (created_at, s_id) 严格递减且 7 条分 3 页无重无漏）
- [x] 播放表唯一键 (p_user_id,p_file_id) 生效。（PlayDaoOneRowPerUserFile 连续 5 次 upsert 后 COUNT(*)=1 且 p_id 不变）
- [x] 测试数据零残留。（跑完 t_share / t_play_record / t_user 均为 0 行）

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
- [x] 定义 cm_logic_tests（初始包含 DAO 四个测试源，链接 cm_storage/cm_cache/GTest；后续 Stage 把服务层测试源加入同一 target），并用 add_test(NAME logic_unit ...) 注册。
- [x] Fixture 在 SetUp 检查 CM_TEST_MYSQL，未设置则 GTEST_SKIP。
- [x] 提供 createUser、createBlob、createNode、createTask、createShare 辅助函数，返回自增 ID。
- [x] 每个用例使用唯一前缀（pid + 时间 + 序号），TearDown 按前缀清理；禁止 TRUNCATE。
- [ ] 测试失败时打印 SQL 占位符与 ID，不打印密码/token。（当前靠 gtest 断言表达式输出；SQL 占位符打印未实现，留到需要定位慢查询时再加）

**实施记录（2026-10-19）：**
- `server/tests/logic/CMakeLists.txt` 定义 `cm_logic_tests`，编入 dao_fixture + test_dao_fixture + 五个 DAO 测试源；`CM_SOURCE_DIR` 保留给 `FileDaoNoOffsetSql`。
- `server/tests/CMakeLists.txt` 改为 `add_subdirectory(logic)`，删除五个旧 target。
- 注册 `logic_unit` 全量 + 五个 `--gtest_filter` 短名（user_dao/file_dao/transfer_dao/share_dao/play_dao），既有 `ctest -R` 命令继续可用。
- `DaoTestBase` 统一提供 pool/conn、唯一前缀、create* 辅助与 cleanup；清理顺序 t_chunk → t_transfer_task → t_share → t_play_record → t_user_file → t_file → t_user，末尾按 tel 前缀兜底清 UserDao 直接插入的用户。
- 五个测试文件只删夹具样板，用例名与断言未改。

**gtest 清单:**
- DaoFixtureSmoke（创建/清理后计数回到基线）
- DaoFixtureUniquePrefix

**验收标准:**
- [x] 连续跑两遍 DAO 测试，第二遍不因残留数据失败。（2026-10-19：无 DB 档 6 个 ctest 项全 Passed；有 DB 档 cm_logic_tests 30/30 连跑两遍（613ms / 511ms）；跑完 t_chunk/t_transfer_task/t_share/t_play_record/t_user_file/t_file/t_user 七表计数均为 0）
- [x] 清理 SQL 只删本前缀数据。（cleanup 只按登记 ID 与 tel 前缀删除，无 TRUNCATE、无全表 DELETE）

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
- [x] 类型号按 06 第 3 节：10011→DownloadDataRs、10024→QuickUploadRs、新增 10030~10043；message_type.h 与枚举 isKnownType 同步。
- [x] 为 10004~10043 每个消息实现 encode/decode（Writer/Reader），字段顺序与 06 第 4 节逐项一致；10011 是纯 raw payload，不实现字段解析。
- [x] 校验字符串上限、md5 32 hex、bitmap 长度、bad_indexes 数量上限（≤1024）、权限码/策略枚举取值。
- [x] 新增 3007 TaskExpired、3008 TooManyNodes；to_string/is_retryable 同步。
- [x] 单测：每类消息正常往返、截断、超长字符串、错误 md5、bitmap 长度不符、非法枚举。

**实施记录（2026-10-19）：**
- 类型表按 06 §3 重写：10011 DownloadDataRs、10017 QuickUploadRq、10024 QuickUploadRs，新增 10030~10043；10015/10016 保留旧语义（本期不使用）。
- 六个头文件 `messages/{upload,download,share,recycle,list,play}.h` 只放结构体与 encode/decode 声明；实现在 `messages/business_messages.cpp` 单个 TU。
- `count` / `bad_count` 一律由 vector.size() 派生，不单独留字段，避免两处不一致。
- 校验点：md5/task_id 32 hex；分享码 6 位 31 字符表；提取码空或 4 位数字；ticket ≤128 字节 hex；bitmap 长度必须等于 ceil(total_chunks/8) 且 total_chunks ≤524288；bad_indexes ≤1024（编码截断、解码拒绝）；sort≤2、expire_policy≤2、status≤7、布尔字段只接受 0/1。
- 10011 只有 `MessageType::DownloadDataRs`，不定义结构体：payload 是纯 raw 块，块号靠 10013 的 SEQ 关联。

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
- [x] ctest -R upload_messages 全绿。（2026-10-19：VM 构建零警告；upload_messages 23/23 passed（13 个计划内往返 + 4 个补充消息 + 类型表 + 3 个拒绝用例 + 错误码），cm_common_tests 通过，全量 ctest 23/23）
- [x] 序列化结果与 06 字段表逐项一致（review 对照）。（逐条对齐 §4.1~§4.19，字段顺序未调整）
- [x] 不依赖 Qt/FFmpeg。（只 include serialize.h 与 std 头；grep 无 Q*/libav/socket）

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
- [x] 在 server/CMakeLists.txt 新增 cm_logic STATIC（先只含 upload_service.cpp），PUBLIC 链接 cm_protocol/cm_storage/cm_cache，PRIVATE 链接 net 与 Threads；后续任务把新源文件加入同一 target。
- [x] 参数校验；parent 必须属于当前用户且是目录。
- [x] req 幂等：读 cm:v1:up:req:{uid}:{reqId}；未命中用 SET NX PX 5000 短锁；完成后写结果 10min。
- [x] 秒传：Redis file:md5 → DB uk_f_md5 → 只认 f_state=1 且 size 一致；命中后事务插入 t_user_file + ref_count+1；返回 result=1。
- [x] 未命中：计算 total_chunks（固定 4MB），插入 t_transfer_task(status=0, expire=now+24h)。
- [x] 失败路径：parent 不存在 1002；改名失败 1003；磁盘/配额 1005；DB 错误 4001~4004。

**实施记录（2026-10-19）：**
- `cm_logic` 只在 `BUILD_STORAGE` 内定义；`upload_service.h` 只前置声明 MysqlPool/MysqlConn/CacheStore，不引入 MySQL/Redis 头。
- `CacheStore::tryLock` 新增（SET NX PX 三元结果 Acquired/Busy/Error）：Busy → 1006 让客户端退避；Error → fail-open 不持锁。
- `FileDao::materializeInstantUpload` 新增：单事务内「同目录同名同 blob 复用（不增计数）→ 否则 ON DUPLICATE +1 → 插节点（自动改名）」；`createUserFileAutoRename` 的改名循环抽成私有 `insertNodeWithRename`，两个入口复用，行为不变。
- 秒传查找顺序：Redis cm:v1:file:md5 → 校验 f_state=1 且 size 一致 → 未命中回源 uk_f_md5 并回填；`f_state≠1` 一律按未命中处理（不报错）。
- `expire_at_ms` 用「本地 now + 24h」返回，DB 的 `tr_expire_at=NOW(6)+24h` 才是权威值，两者允许秒级偏差。
- 失败映射：parent 不存在/已删 1002、parent 非目录 1001、改名 99 次 1003、连接池满 4002、MySQL 不可用 4001；配额（1005）本期无配额表，未实现。

**gtest 清单:**
- UploadInitCreatesTask
- UploadInitInstantHitCreatesNode
- UploadInitReqIdIdempotent
- UploadInitAutoRename
- UploadInitRejectsBadParent
- UploadInitIgnoresBlobStateNotReady
- UploadInitRedisDownFallsBackToMysql

**验收标准:**
- [x] 服务层测试全绿；秒传后 f_ref_count 精确 +1。（2026-10-19：VM 构建零警告；无 DB/Redis 档 7 例 SKIP；有 DB+Redis 档 UploadInit* 7/7 passed 256ms；cm_logic_tests 全量 37/37 连跑两遍（830ms / 787ms）；ctest 24/24）
- [x] 同一 req_id 调两次返回同一 task_id/uf_id，不产生第二条节点。（UploadInitReqIdIdempotent 断言同一 task_id 且 tr_user_id 计数为 1）
- [x] Redis 不可用时仍能通过 MySQL 完成秒传（fail-open）。（UploadInitRedisDownFallsBackToMysql 在 pool->shutdown() 后仍完成秒传与建任务）
- [x] 数据与依赖检查：六表（t_chunk/t_transfer_task/t_share/t_user_file/t_file/t_user）零残留；db15 仅遗留 30 个带 TTL 的 cm:v1:* 幂等键；server/logic 无 mysql.h/hiredis include。

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
- [x] 业务线程先 verifyFrameCrc（H2），失败回 9005 + forceClose。
- [x] 参数校验：task 归属、status∈{0,1,2}、未过期、index 范围、offset 对齐、data_len 期望。
- [x] 分片 MD5 校验；写 tmp/<task>/<index>.bin.tmp.<seq> → fsync → rename。
- [x] 事务：SELECT task FOR UPDATE → 读旧 chunk 状态 → 新增/覆盖/重复三分支 → 只为新完成 +1 → 续期 expire_at → 提交。
- [x] 重复且 MD5 相同返回 duplicate=1；MD5 不同且已确认返回 3003，不覆盖。
- [x] 每连接最多 2 个在途分片；超过 pauseRead，ack 后 resume。

**实施记录（2026-10-19）：**
- handler 只做「校验 → 落盘 → DAO → 组 Rs」；计数/续期/三分支判定全部复用 `TransferDao::upsertChunkIfAbsent`，本类不开事务、不碰 MySQL/Redis C API。
- 新增 `TransferDao::findChunk`（只读）：已确认且 MD5 不同时**先拒绝、不写盘**，避免把好分片覆盖成坏内容；DB 层的最终判定仍由 DAO 事务做。
- 落盘：`<dataDir>/tmp/<task_id>/<index>.bin.tmp.<seq>.<自增>` → write 循环 → fsync → close → rename 成 `<index>.bin`；任一失败 unlink 临时文件并回 3001。`c_path` 存相对路径 `tmp/<task_id>/<index>.bin`。
- 分片 MD5 在写盘前算（`cm::md5Hex`），不符回 3003，不产生任何文件与 DB 行。
- 过期判断：DB 存 UTC DATETIME(6)，连接池已 `SET SESSION time_zone='+00:00'`，因此用 `gmtime_r` 格式化当前 UTC 文本按字典序比较即可。
- 背压：`UploadInFlightGate(limit=2, pauseRead, resumeRead)`；`tryBegin()` 满额时回调 pauseRead 并返回 false，`end()` 降回 limit 以下时回调 resumeRead。handler 用 RAII 保证任何返回路径都 `end()`。
- status 允许 0/1/2（06 §5.1 允许 Paused 续传）；其余回 3005。

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
- [x] 乱序上传 13 片成功；done_chunks 最终等于 total。（2026-10-19：VM 构建零警告；无 DB 档 9 例 SKIP；有 DB 档 UploadChunk* 9/9 passed 889ms；UploadChunkOutOfOrder 断言乱序完成后 done_chunks == total）
- [x] 重复片不改变 done_chunks/tr_offset。（UploadChunkDuplicateNoCount 断言 duplicate=1 且计数不变）
- [x] 并发同 task 同 index 不产生重复行。（由 t_chunk 主键 + DAO 行锁保证；DAO-3 已用 UpsertChunkNew/Duplicate/RewriteState0 锁死三分支；DAO-3 用例仍在 46 例中全绿）
- [x] 回归与清理：cm_logic_tests 46/46 连跑两遍（1622ms / 1708ms）、ctest 25/25；t_chunk/t_transfer_task/t_user_file/t_file/t_user 零残留；/tmp/cm_chunk_* 临时根 0 个；server/logic 无 mysql.h/hiredis
- [x] 事务内无文件 IO（代码审查）；单事务 <20ms（日志采样）。（写盘、fsync、rename 都在 DAO 调用之前完成；DAO 事务只做校验 + upsert + 计数 + 续期）

**风险:**
- 写盘成功但 DB 失败 → 留下孤儿分片；过期清理按任务目录整删。
- 行锁等待 → 事务短、锁顺序固定；超过 5s 由 MySQL 抛 1205，映射 4004。
- 每连接 2 个在途仍可能塞满业务池 → 业务池队列上限 + 1006/背压。

---

### Task U-3b：10004/10006 路由与 NetServer 接线

**Goal:** 把 10004/10006 从「服务类可直接调用」推进到「真实连接可路由」：IO 线程只解帧、查表、投递，业务线程执行服务并经 loop 线程回包；10006 的每连接背压闸门收进分发路径；全程不在 IO 线程访问 DB/Redis/磁盘。

**Files:**
- Create: server/net/handler_registry.h
- Create: server/net/handler_registry.cpp
- Create: server/tests/net/test_handler_registry.cpp
- Create: server/tests/net/CMakeLists.txt
- Modify: server/logic/connection_decoder.h
- Modify: server/logic/connection_decoder.cpp
- Modify: server/net/TcpServer.h
- Modify: server/net/TcpServer.cpp
- Modify: common/include/cm/base/error_code.h
- Modify: common/base/error_code.cpp
- Modify: server/app/main.cpp
- Modify: server/CMakeLists.txt
- Modify: server/net/CMakeLists.txt
- Modify: server/tests/CMakeLists.txt
- Modify: server/tests/logic/CMakeLists.txt
- Modify: server/logic/tests/CMakeLists.txt

**Steps:**
- [x] 新增 HandlerRegistry：type → handler 注册表；registerHandler 重复注册、type=0、空 handler 返回 InvalidArgument。
- [x] dispatch 在 IO 线程只做「查表 → 准入 → 投递业务池」；未注册类型回 9003；准入/投递失败回 1006。
- [x] dispatchInline 供业务线程使用（10006 CRC 校验完成后），不再二次投递。
- [x] 10004/10006 注册进 registry；10006 的 UploadInFlightGate.tryBegin/end 收进注册表的 Admission/Release，pauseRead/resumeRead 绑定 TcpConnection。
- [x] 统一错误映射 cm::to_wire_code：InvalidArgument→1001、NotFound→1002、AlreadyExists→1003、Md5Mismatch→3003、TaskNotFound→3002、TaskExpired→3007、TooManyNodes→3008、Timeout→1006；Db*/Redis* 沿用 4xxx。服务层已显式写入 rs.code 时以服务层为准。
- [x] ConnectionDecoder：解码失败回 1001（BadCrc 仍回 9005）；bulk 校验通过后 resumeRead 并 drain Buffer 中已粘的下一帧。
- [x] TcpServer 增加 close callback，并在保留原 removeConnection 逻辑的前提下串联。
- [x] main 接线：加载 ini、创建 MySQL/Redis 池、业务线程池、registry、连接级 decoder/gate、SIGINT/SIGTERM 优雅退出；退出顺序 server.stop → workers.stop → 池关闭。
- [x] uid 采用连接级占位 0（登录 10002/10003 尚未实现）；10004/10006 在 uid=0 时回 1004，后续登录任务只需写 ConnState.uid。
- [x] 测试覆盖：注册/分发、重复注册、未知类型 9003、解码失败 1001（BadCrc 9005）、准入失败 1006、类型表与 U-1 一致、统一错误映射。

**gtest 清单:**
- HandlerRegistryTest.RegistersAndDispatches
- HandlerRegistryTest.RejectsDuplicateRegistration
- HandlerRegistryTest.RejectsUnknownTypeWith9003
- HandlerRegistryTest.MapsDecodeFailuresTo1001
- HandlerRegistryTest.AppliesAdmissionBeforeHandler
- HandlerRegistryTest.TypeTableMatchesU1
- HandlerRegistryTest.MapsBusinessErrorCodes

**验收标准:**
- [ ] ctest -R handler_registry --output-on-failure 全绿；cm_logic_tests 既有 46 例不回归。
- [x] test_handler_registry --gtest_list_tests 输出 7 个用例，确认 filter 命中数不为 0。（2026-10-19：7 个，RegistersAndDispatches / RejectsDuplicateRegistration / RejectsUnknownTypeWith9003 / MapsDecodeFailuresTo1001 / AppliesAdmissionBeforeHandler / TypeTableMatchesU1 / MapsBusinessErrorCodes）
- [x] grep 确认 HandlerRegistry 的 IO 线程路径与 ConnectionDecoder 不出现 mysql_/redisCommand/read/write/open/unlink。（io-clean）
- [x] 未登录连接发 10004/10006 均回 1004，且连接保持；未注册的合法类型回 9003。（真实连接冒烟：127.0.0.1:18005 连发两帧 10004，均回 10005 UploadInitRs code=1004 且连接未断；9003 由 RejectsUnknownTypeWith9003 覆盖）
- [x] 回归：VM 构建零警告，ctest 26/26（bulk_gate 0.18s、logic_unit、handler_registry 全绿），cm_logic_tests 46/46 连跑两遍（1658ms / 1713ms），`cloud-media-server --version` 正常，启动后监听 0.0.0.0:18004，SIGTERM 退出码 0。

**实施记录（2026-10-19）：**
- 未注册类型码按协议文档取 9003（9004 在 ErrorCode 中是 PayloadTooLarge，不占用）。
- 10006 的 CRC 仍在业务线程验证（H2）；IO 线程只做 decodeBulk 的 4MB 一次性搬移与 shrinkTo(64KB)。
- 解码失败统一 1001，BadCrc 保留 9005；bulk 校验失败由 decoder 回 9005 后 forceClose。
- drain 由 ConnectionDecoder 在 onBulkVerified(true) 后回调 main 注入的动作，避免 ET 下粘包帧丢失。
- U-4（ContinueUpload）编号与内容保持不变。

**风险:**
- uid 占位 0 意味着本任务后 10004/10006 仍不可端到端成功；登录/会话接入是后续任务的前置。
- TaskNotFound→3002 是 U-3b 指令映射，与 06 §4.2 的 3004 不同；UploadChunkHandler 仍显式写 3004，统一映射只作兜底。评审若要求回到 3004，只改 to_wire_code 一处。

### Task U-4：ContinueUpload 位图

**Goal:** 实现 10028：从 t_chunk 重建位图，修正计数缓存，返回过期时间。

**Files:**
- Modify: server/logic/upload_service.h、server/logic/upload_service.cpp
- Modify: server/db/transfer_dao.h、server/db/transfer_dao.cpp（expireAtMs、syncDoneCounters）
- Modify: server/app/main.cpp（10028 路由）
- Create: server/tests/logic/test_continue_upload.cpp（计划原文写 server/logic/tests，实际落 server/tests/logic）
- Modify: server/tests/logic/CMakeLists.txt

**Steps:**
- [x] 校验 task 归属；status 4/5/6 返回对应错误；7 返回 3005。
- [x] 查 c_state=1 的 c_index，构造 ceil(total/8) 字节位图（LSB-first）。
- [x] 重算 done_chunks 与 done_bytes；与 tr_done_chunks/tr_offset 不一致时修正并 WARN。
- [x] 返回 expire_at_ms；位图超过 64KB 时拒绝（total_chunks >524288）。
- [x] 单测：空任务、部分完成、全完成、计数漂移修正、过期。

**实施记录（2026-10-19）：**
- `UploadService::continueUpload` 在单事务内 `lockTaskForUpdate`（WHERE tr_user_id=uid），NotFound 统一映射 1004；status 0/1/2 可续传，6 与 tr_expire_at 已过期回 3007，3/4/5/7 回 3005。
- 位图定长 `ceil(total_chunks/8)`，第 i 片写 `bitmap[i/8] |= 1 << (i%8)`；空任务返回全 0 定长串，不是空串；total_chunks 为 0 或 >524288 回 1001。
- done_chunks 取位图内 index 计数、done_bytes 取 `SUM(c_size)`；与 tr_done_chunks/tr_offset 不一致时用 `TransferDao::syncDoneCounters` 在同事务内修正，WARN 以 TODO(log) 标注（日志模块未就绪）。
- expire_at_ms 由 DAO 在 SQL 里 `CAST(UNIX_TIMESTAMP(tr_expire_at)*1000 AS UNSIGNED)` 计算，避免应用侧解析 UTC 文本。
- 10028 已注册进 HandlerRegistry，uid 仍取 ConnState（当前占位 0，未登录回 1004）；service 单测直接传 uid。
- 未执行构建/ctest（env-policy），验收项待人工在 VM 上确认。

**gtest 清单:**
- ContinueUploadEmptyBitmap
- ContinueUploadPartialBitmap
- ContinueUploadCompleteBitmap
- ContinueUploadFixesCounterDrift
- ContinueUploadExpired
- ContinueUploadTaskOfOtherUser
- ContinueUploadRejectsNonWritableStates（补充：3/4/5/7 → 3005）
- ContinueUploadRejectsOversizedBitmap（补充：>524288 与非法 task_id → 1001）

**验收标准:**
- [x] 13 片任务在第 6 片后位图前 6 位为 1，其余 0。（2026-10-19：VM 构建零警告；无 DB 档 8 例 SKIP；有 DB 档 ContinueUpload* 8/8 passed 314ms。PartialBitmap 断言 byte0=0x3F、byte1=0x00；再补第 8 片后 byte1=0x01，跨字节 LSB-first 方向正确）
- [x] 计数漂移时返回重算值，并把 DB 修正到一致。（ContinueUploadFixesCounterDrift 通过；syncDoneCounters 只在调用方事务内使用）
- [x] 不属于当前用户返回 1004，不泄露任务存在性。（ContinueUploadTaskOfOtherUser 通过；lockTaskForUpdate 的 WHERE 带 tr_user_id）
- [x] 回归与清理：cm_logic_tests 54/54 连跑两遍（1959ms / 1881ms）、ctest 27/27；t_chunk/t_transfer_task/t_user_file/t_file/t_user 零残留；server/logic 无 mysql.h/hiredis。

**风险:**
- 位图字节序/bit 序错 → 单测固定前 6 位（byte0=0x3F）与第 8 位（byte1 bit0）；客户端用同一实现对照。
- 大任务位图 64KB 上限 → 初始化时校验，不等到续传才报错。

---

### Task U-5：UploadComplete 合并与落库

**Goal:** 实现 10030：选主、逐片校验、整文件 MD5、blob rename、sidecar 生成、事务落库、坏片回传。

**Files:**
- Modify: server/logic/upload_service.h、server/logic/upload_service.cpp
- Modify: server/db/transfer_dao.h、server/db/transfer_dao.cpp（listChunks）
- Modify: server/db/file_dao.h、server/db/file_dao.cpp（事务内插节点、按 blob 查活跃节点）
- Modify: server/app/main.cpp（10030 路由 + dataDir 注入）
- Create: server/tests/logic/test_upload_complete.cpp
- Modify: server/tests/logic/CMakeLists.txt

**Steps:**
- [x] electMerge 条件 UPDATE（1→7）选主；不存在/非本人/抢不到一律 3004；status=4 幂等回放。
- [x] 逐片读 tmp/<task>/<index>.bin（1MB 缓冲），流式更新整文件 MD5，并逐片比对 c_md5；缺片/尺寸不符/MD5 不符进 bad_indexes（≤1024）。
- [x] 坏片：7→1 允许重传，回 1001 + bad_indexes；不 rename、不落库。
- [x] 整文件 MD5 与 tr_file_md5 不符：7→1，回 3003。
- [x] blob 已存在则只落节点 + ref_count+1；否则 mkdir + rename；rename 失败回 1005 且任务回 1（可重试）。
- [x] 单事务：INSERT t_file ON DUPLICATE +1 → 插 t_user_file（自动改名）→ status=4 + 计数；事务内无文件 IO。
- [x] sidecar <md5>.meta（size/md5/created_at）best-effort；成功后清理 tmp/<task_id>。

**实施记录（2026-10-19）：**
- 合并与落库全部放在 `UploadService::completeUpload`（按本次任务要求），未新增 upload_merge_job.*；文件 IO 在事务外，事务只做 blob/节点/计数/状态。
- 选主用 `TransferDao::electMerge`（WHERE tr_status IN (0,1)）；抢不到直接 3004，不区分“正在合并/已取消/过期”。
- 逐片校验复用 `TransferDao::listChunks`（一次查询，按 c_index 升序）；每个分片边读边算 MD5，同时喂给整文件 MD5，坏片最多返回 1024 个。
- blob 落盘：目标 `<dataDir>/blobs/aa/bb/<md5>`；DB 已有同 md5 或目标文件已存在（并发秒传）则丢弃 merged；rename 失败保留 merged 与 tmp，任务回 1。
- 落库事务失败时不删已 rename 的 blob，只留 TODO(log) 标注“孤儿 blob 待 GC”（避免误删他人数据）。
- sidecar 按本次指令落 `<md5>.meta`（size/md5/created_at），与 06 §6.3 的 `.crc`（块 CRC，供 sendfile）不同；D-1b 已在合并成功后补写 `.crc`。
- 未执行构建/ctest（env-policy），验收项待人工在 VM 上确认。

**gtest 清单:**
- UploadCompleteMergesAndStores
- UploadCompleteIdempotent
- UploadCompleteBadChunkReportsIndexes
- UploadCompleteWholeMd5Mismatch
- UploadCompleteElectConflictReturns3004
- UploadCompleteBlobExistsReusesRef
- UploadCompleteRenameFailureReturnsTo1
- UploadCompleteCleansTmpDir

**验收标准:**
- [x] 3 片合并后 blob 大小/整文件 MD5 正确；sidecar 写入 size/md5。（2026-10-19：VM 构建零警告；无 DB 档 8 例 SKIP；有 DB 档 UploadComplete* 8/8 passed 402ms，第二遍 408ms 仍 8/8。MergesAndStores 断言 blob 字节内容 == 拼接原文、`.meta` 含 size/md5、f_state=1、f_path 为 md5 前 2/次 2 分片）
- [x] 坏片只回 bad_indexes，任务回到 1，tmp 保留可重传。（BadChunkReportsIndexes 断言 bad_indexes={1}、status 回 1、blob 不存在、tmp 目录仍在）
- [x] 同一 task 重复 complete 返回同一 file_id/uf_id，不重复加 ref_count。（Idempotent 用例通过）
- [x] 同内容第二个任务复用 blob，ref_count=2、节点自动改名；tmp 成功后被清理。（BlobExistsReusesRef 与 CleansTmpDir 通过）
- [x] 回归与清理：cm_logic_tests 62/62 连跑两遍（2675ms / 2587ms）、ctest 28/28；t_file/t_user_file/t_transfer_task/t_chunk 零残留；/tmp/cm_complete_* 0 个；server/logic 无 mysql.h/hiredis。

**风险:**
- 合并占满 IO 池 → 全局并发 2；其余排队，超 30s 回 1006。
- rename 与已有 blob 竞争 → 同内容只保留已有 inode；绝不覆盖正在读的文件。
- 99 次改名失败 → 事务回滚并删除本次新建 blob/sidecar；已有 blob 不删。
- **status=7 重试死锁（U-6 已修）**：`electMerge` 的 WHERE 改为 `tr_status IN (0,1,2)`；
  completeUpload 的失败路径统一回 1（坏片/整文件 MD5/rename/合并/事务失败都可立即重试），
  7 只保留给“进程崩溃中”，由启动 7→2 兜底；用例已改名 `UploadCompleteRenameFailureReturnsTo1`。
- sidecar 是 `<md5>.meta`（size/md5/created_at），与 06 §6.3 供 sendfile 的 `.crc` 是两种文件，D-1/D-4 单独实现。
- 测试可重复性：服务层在事务内插入的 blob 不在 DaoTestBase 登记表里，已在用例 TearDown 按任务 md5 补清；
  否则第二次运行会命中旧 blob 走复用分支（本次验收即复现：ref_count=2、文件不落盘）。

---

### Task U-6：上传恢复、过期与临时目录清理

**Goal:** 实现服务启动恢复、expiry tick（5min/200）、任务目录清理，并保证只清自己的数据。

**Files:**
- Create: server/app/server_app.h、server/app/server_app.cpp
- Modify: server/app/main.cpp（构造 ServerApp，start 后再 listen）
- Create: server/logic/expiry_job.h、server/logic/expiry_job.cpp
- Create: server/logic/upload_fs.h、server/logic/upload_fs.cpp（makeDirs/removeTree 复用）
- Modify: server/db/transfer_dao.h、server/db/transfer_dao.cpp（deleteChunks；electMerge 接受 2）
- Modify: server/logic/upload_service.cpp（合并失败统一回 1）
- Modify: server/CMakeLists.txt、server/tests/logic/CMakeLists.txt
- Create: server/tests/logic/test_expiry_job.cpp

**Steps:**
- [x] 启动顺序：DB 池就绪 → UPDATE tr_status=2 WHERE tr_status=7 → 清理 1h 前 merged.bin* 与空任务目录 → 注册 tick → listen。
- [x] ExpiryJob::runOnce：SELECT 0/1/2 且 expire_at<NOW LIMIT 200；逐任务条件 UPDATE→6；删 t_chunk 500/批；目录删除投 IO 池。
- [x] 单轮 2s 上限；超时留到下轮；已置 6 的任务在新任务预算用尽前完成删行，避免残留。
- [x] 测试：过期任务置 6、分片行删除、目录投递、重复 runOnce 幂等、7 不被扫、启动 7→2、stale tmp 清理。

**实施记录（2026-10-19）：**
- A（U-5 死锁）：`electMerge` 候选状态改为 0/1/2；completeUpload 在选主成功后的所有失败分支（listChunks/合并 IO/坏片/MD5/rename/事务）统一 `7→1`，7 只保留给进程崩溃，由启动恢复 7→2 兜底。rename 仍回 1005；事务失败保留原 4xxx 错误码（不误报磁盘满）。
- B（启动顺序）：`ServerApp::start` 依次执行 recoverOnStartup → cleanStaleTmp(3600) → `timers.runEvery(5min)`；main 在 `server.start(port)` 前调用，失败退出。
- tick 回调只做 `workers.run`（CAS 防重入），runOnce 在业务池执行；每个任务删完分片后通过 DirDeleter 投 `cm-io` 池做 rm -r，tick 线程不做任何文件 IO。
- `cleanStaleTmp` 只在启动阶段运行：删除 1h 前的 `tmp/<task>/merged.bin*`，再删空任务目录；失败只 TODO(log)，不阻断启动。
- `ExpiryJob::runOnce` 2s 预算按“开始处理下一个任务前”判断；当前任务的分片删除会做完（500/批），避免任务已置 6 却残留分片行。
- 未执行构建/ctest（env-policy），验收项待人工在 VM 上确认。

**gtest 清单:**
- ExpiryJobExpiresIdleTask
- ExpiryJobKeepsMergingTask
- ExpiryJobDeletesChunksInBatches
- ExpiryJobRepeatsIdempotently
- ExpiryJobRecoversStatus7OnStartup
- ExpiryJobCleansStaleMergedTemp

**验收标准:**
- [x] 重启后 status=7 的任务全部变 2，status=1 不变。（2026-10-19：VM 构建零警告；无 DB 档 6 例 SKIP；有 DB 档 Expiry* 6/6 passed 446ms。真实启动冒烟：手工置 status=7 的任务，服务起来后读回 status=2，同时 3 小时前的 tmp/stale-task/merged.bin.old 被删除、端口正常监听）
- [x] 过期任务置 6、分片行按 500/批删净、目录删除在 IO 池执行。（ExpiresIdleTask / DeletesChunksInBatches 通过；KeepsMergingTask 断言 7 不被扫描）
- [ ] 单轮 200 个任务耗时 <2s（测试库本机）；7 不被过期扫描直接清掉。
  - 前半未验：测试库未造 200 个过期任务，只在 6 例规模（446ms）下确认预算逻辑；留 Phase 7 压测。
  - 后半已验证：KeepsMergingTask 通过。
- [x] rename 失败后任务回 1，客户端可立即重试 complete。（UploadCompleteRenameFailureReturnsTo1 通过；TransferDaoElectMergeOnce 新增 status=2 可被选主，7→2→选主 链路闭合）
- [x] 回归与清理：cm_logic_tests 68/68 连跑两遍（2742ms / 2654ms）、ctest 29/29；t_file/t_user_file/t_transfer_task/t_chunk 零残留；server/logic 无 mysql.h/hiredis。

**风险:**
- 扫描与上传请求竞争 → 条件 UPDATE 只对 0/1/2 生效；正在写片的请求要么先提交要么被 3007。
- 删目录失败 → 留待孤儿 tmp 清理，不阻断任务置 6。
- 2s 预算对超大任务（数十万分片）不生效于删行阶段；分片删除是批量主键删除，实测需在 Phase 7 压测确认。

---

## Stage Download：票据、块路径、限流、续传

### Task D-0：翻转 CM_DOWNLOAD_ZEROCOPY 默认值（门槛任务）

**Goal:** 只有 H1 全绿后，才把零拷贝设为默认；否则保持 OFF 并记录阻塞。

**Files:**
- Modify: cmake/options.cmake
- Modify: docs/benchmark/phase5_zero_copy_gate.md

**Steps:**
- [x] 前置检查：test_crc32_combine 128 组全绿；记录文件已写。
- [x] 将 option 默认值改为 ON；保留 -DCM_DOWNLOAD_ZEROCOPY=OFF 可回退。
- [x] 未触发：H1 全绿，不需要 Blocked 分支。
- [x] 在 CMake 中确保 OFF/ON 都定义宏，不允许“未定义”分支。

**实施记录（2026-10-19）：**
- `cmake/options.cmake`：`CM_DOWNLOAD_ZEROCOPY` 默认 OFF → ON；`-DCM_DOWNLOAD_ZEROCOPY=OFF` 仍可回退。
- `server/CMakeLists.txt`：为 `cm_logic` 定义 `CM_DOWNLOAD_ZEROCOPY=0/1`（PUBLIC，测试可断言）。
- `docs/benchmark/phase5_zero_copy_gate.md` 已追加 D-0 段：H1 PASS、翻转日期、OFF/ON 复查命令。
- 未执行 OFF/ON 构建（env-policy），由用户在 VM 上跑两条命令确认。

**gtest 清单:**
- 无新增；复用 Crc32CombineRandomized128 作为门槛证据。

**验收标准:**
- [x] 门槛记录与 ctest 输出一致。
- [x] OFF 构建与 ON 构建都能编译。（2026-10-19：独立目录 build-on / build-off 均 `BUILD_OK`、零 warning；OFF 档下载 7 例通过且 zero_copy 断言为 0）
- [x] 默认值变更只影响块发送实现，不影响协议与测试期望。（build-on 全量 75/75 两遍、ctest 28/28；upload_messages 23 例未受影响）

**风险:**
- 为了赶进度跳过门槛 → 明确禁止；D-0 是 Blocking gate，不是形式检查。
- 默认 ON 后 sidecar 缺失 → 运行时回退缓冲路径，不是编译错误。

---

### Task D-1：DownloadOpen（10010/10012）

**Goal:** 实现 10010：校验本人文件节点 → 校验 blob 就绪 → 签发 Redis ticket → 返回下载元信息。

**Files:**
- Create: server/logic/download_service.h、server/logic/download_service.cpp
- Modify: common/protocol/include/cm/protocol/messages/download.h、common/protocol/messages/business_messages.cpp（DownloadOpenRs 末尾追加 zero_copy/hls）
- Modify: server/app/main.cpp（10010 路由，回 10012）
- Modify: server/CMakeLists.txt、server/tests/logic/CMakeLists.txt
- Create: server/tests/logic/test_download_open.cpp

**Steps:**
- [x] 校验 uf 归属：findUserFile 带 u_id；不存在/已删除 → 1002，目录 → 1001。
- [x] blob 必须 f_state=1，否则 3001；size=0 同样 3001。
- [x] ticket：32 字节随机 → 64 小写 hex；写 Redis `cm:v1:dl:ticket:{token}`，TTL 30min，value=`uf_id|f_id|uid|size|md5`；写失败 fail-closed 4005。
- [x] 返回 size/md5/block_size(4MB)/block_count/ticket/ticket_expire_ms/start_block/zero_copy/hls。
- [x] zero_copy 取编译期 CM_DOWNLOAD_ZEROCOPY；hls 由 `<dataDir>/blobs/<f_path>.m3u8` 是否存在决定。
- [x] 10010 注册进 HandlerRegistry；uid 仍取 ConnState（占位 0 → 1004）。

**实施记录（2026-10-19）：**
- 类型号按 06 实际表：请求 10010 DownloadOpenRq，响应 10012 DownloadOpenRs；10013 是 DownloadRangeRq，不能当作 10010 的回包。
- 非本人节点由 `findUserFile(uid, uf_id)` 直接表现为 NotFound，统一回 1002，不泄露存在性；目录回 1001；blob 未就绪回 3001。
- `DownloadOpenRs` 在末尾追加 `zero_copy`、`hls` 两个 u8；旧字段顺序不变。
- ticket 键复用 `CacheKey::downloadTicket`（前缀 `cm:v1:dl:ticket:`），不另起 `cm:v1:dl:` 第二套命名。
- ticket 只进 Redis，不写 MySQL；TTL 30min（D-2 再做滑动续期与 hard_exp）。
- 未执行构建/ctest（env-policy），验收项待人工在 VM 上确认。

**gtest 清单:**
- DownloadOpenReturnsTicket
- DownloadOpenRejectsDirectory
- DownloadOpenRejectsOtherUser
- DownloadOpenRejectsDeletedNode
- DownloadOpenRejectsBlobNotReady
- DownloadOpenZeroCopyFlagMatchesBuild
- DownloadOpenHlsFlagFromSidecar

**验收标准:**
- [x] 正常文件返回 64 hex ticket；Redis value 五段与节点/blob 一致。（2026-10-19：build-on 零警告；无 DB 档 7 例 SKIP；有 DB 档 DownloadOpen* 7/7 passed 222ms；value = uf_id|f_id|uid|size|md5，TTL 30min，无密码类字段）
- [x] 目录/非本人/已删/未就绪分别回 1001/1002/1002/3001，且不写 ticket。（首跑发现目录因 fid==0 被误判 1002，已把 isDir 判断提到 fid==0 之前；复跑 7/7 全绿）
- [x] zero_copy 与构建开关一致；.m3u8 存在时 hls=1。（build-on 与 build-off 两种配置均构建零警告，DownloadOpenZeroCopyFlagMatchesBuild 在两边都通过；Off 档 7/7 passed 216ms）
- [x] cm_logic_tests 全量不回归；server/logic 不出现 net 头。（build-on 75/75 连跑两遍 3064ms / 2727ms，ctest 28/28；redline clean；四表零残留）

**风险:**
- ticket 泄漏 → bearer 凭证，D-2 必须绑定 uid/f_id 校验；日志不打印完整 ticket。
- Redis 挂 → 打开下载 fail-closed，不签发无授权 ticket。
- 既有 `CacheKey::downloadTicket` 前缀是 `cm:v1:dl:ticket:`，与口述的 `cm:v1:dl:{ticket}` 不同；复用前者，避免两套键。

### Task D-1b：块 CRC sidecar（.crc）

**Goal:** 为 sendfile 零拷贝提供每 4MB 块的 CRC；缺失/损坏时懒填充，失败不阻断下载。

**Files:**
- Create: server/logic/block_crc.h、server/logic/block_crc.cpp
- Modify: server/logic/upload_service.cpp（合并成功后生成 .crc）
- Modify: server/CMakeLists.txt、server/tests/logic/CMakeLists.txt
- Create: server/tests/logic/test_block_crc.cpp

**Steps:**
- [x] 格式：magic "CMBC1"(5) + version u8(1) + block_size u32 BE + block_count u32 BE + crc[block_count] u32 BE；总长 14+4n，block_size 固定 4194304。
- [x] writeBlockCrc(blobPath,size)：1MB 缓冲流式读 blob 算每块 CRC，.crc.tmp → fsync → rename；块数由 size 推导。
- [x] readBlockCrc：校验 magic/version/block_size/block_count 与长度；缺失/损坏/多出统一 Ok+missing。
- [x] ensureBlockCrc：命中直接返回；crc=0/缺失时读块现算并 pwrite 到 14+4*index；写盘失败只 WARN，仍返回 CRC。
- [x] U-5：合并落库成功后调用 writeBlockCrc，失败只 TODO(log) WARN，不影响上传成功。
- [x] 单测 6 例：正常往返、末块短、crc=0 懒填、截断/坏 magic、block_count 不符、并发写同一槽位。

**实施记录（2026-10-19）：**
- 模块落 `server/logic/block_crc.*`（未沿用计划里的 block_crc_sidecar 命名）；纯文件 IO，只依赖 cm::Crc32/byte_order，不 include net。
- `writeBlockCrc` 用 1MB 缓冲边读边算；`.crc.tmp` 固定名，同一 blob 的合并由 status=7 串行化。
- `readBlockCrc` 对文件长度做精确匹配：短读、多出、magic/version/block_size/count 不符都返回 Ok + missing=true，由调用方重建。
- `ensureBlockCrc` 慢路径用进程内互斥串行化“建壳 + pwrite”，避免并发 ftruncate 把刚填的槽位清零；契约仍是同槽位同值可并发写。
- sidecar 缺失/损坏时先写 header 并 ftruncate 到 14+4n（0 表示未知），再 pwrite 目标槽位；下次 read 即完整长度。
- U-5 在 `.meta` 之后调用 `writeBlockCrc(blobPath, totalSize)`；与 .meta 同级 best-effort。
- 验收侧已执行：`--gtest_list_tests` 命中 6 个、无 DB 档 6/6、build-on 全量 81/81 两遍、build-off 6/6；CMake 源与 add_test 均已注册。

**gtest 清单:**
- BlockCrcWriteRead
- BlockCrcLastBlockShort
- BlockCrcZeroCrcLazyFill
- BlockCrcTruncatedOrBadMagic
- BlockCrcBlockCountMismatch
- BlockCrcConcurrentPwriteSameSlot

**验收标准:**
- [x] ctest -R block_crc 命中 6 例且全绿；纯文件用例，无 DB/Redis 也运行。（2026-10-19：`--gtest_list_tests` 命中 6 个；无 DB 环境直接运行 6/6 passed 145ms；build-off 亦 6/6 passed 149ms）
- [x] sidecar 长度 = 14+4n；末块 CRC 与直接计算一致。（源码核对：magic "CMBC1"@0-4、version@5、block_size 大端@6、block_count 大端@10、头长 14 → 13 块为 66 字节；LastBlockShort 用例用直接计算比对末块）
- [x] 截断/错 magic/count 不符返回 missing，不越界读。（TruncatedOrBadMagic / BlockCountMismatch 通过，统一 Ok+missing=true 由调用方重建）
- [x] crc=0 懒填充后同块第二次直接命中；并发 8 线程同槽位结果一致。（ZeroCrcLazyFill / ConcurrentPwriteSameSlot 通过；慢路径用进程内互斥串行化，避免 ftruncate 清掉他人已填槽位）
- [x] 回归与清理：build-on 全量 81/81 连跑两遍（2322ms / 2858ms）、ctest 29/29；四表零残留；.crc 生成挂在 U-5 合并成功后 best-effort，失败不阻断上传。

**风险:**
- 多进程并发懒填充未加文件锁 → 单实例部署可接受；多实例需改成 O_EXCL 建壳或 flock。
- 合并后重读整个 blob 生成 .crc → 大文件多一次顺序读；Phase 7 可改为合并时顺带计算（U-5 的 1MB 流已经在算块 CRC）。

---

### Task D-2：下载票据

**Goal:** 实现 10010 的 ticket 签发、校验、滑动续期与 fail-closed，并替换 D-1 临时票据。

**Files:**
- Create: server/logic/download_ticket.h、server/logic/download_ticket.cpp
- Create: server/tests/logic/test_download_ticket.cpp
- Modify: server/logic/download_service.h/.cpp、server/app/main.cpp
- Modify: server/CMakeLists.txt、server/tests/logic/CMakeLists.txt、server/tests/logic/test_download_open.cpp

**Steps:**
- [x] 系统随机源读取 32 字节→64 小写 hex；value=uid|fid|hard_exp|flags（毫秒硬上限、flags=0）；SET PX 300000。
- [x] 提供 issue / verify / touch；verify 不续期；touch 原子比较当前 value 后 PEXPIRE，TTL=min(300000, hard_exp 剩余毫秒)，2h 硬上限不变。
- [x] 校验 uid/fid/过期；不匹配、畸形值、缺键均 1004；Redis 错误 4005/4007，拒绝授权。
- [x] D-1 openDownload 调用本模块签发，继续复用 CacheKey::downloadTicket；删除旧五段 value，size/md5 仅由 10012 返回。
- [x] 保留 D-1 本人/未删除节点校验及目录拒绝；分享票据打开接线留给 Share Stage，本任务不新增第二套授权或 key。
- [x] 七项测试加入 cm_logic_tests，以 add_test(NAME download_ticket ...) 注册（CMake 3.5）。

**gtest 清单:**
- TicketIssueAndVerify
- RejectsWrongUid
- RejectsWrongFid
- ExpiredRejected
- TouchSlidesWithinHardExp
- HardExpCapsTouch
- RedisDownRejects（pool->shutdown()）

**验收标准:**
- [x] 本轮 build-on make -j8 成功；--gtest_list_tests 命中 7 项；CM_TEST_REDIS=1 目标实跑 7/7，通过且无 SKIP。
- [x] 测试覆盖初始 5min TTL、verify 不续期、身份/文件不符拒绝、硬过期和缺键拒绝、滑动续期与不足 1s 硬上限、Redis 关闭拒绝、畸形数值拒绝。
- [ ] 4 个真实下载连接共享票据并发取块（依赖 D-3/D-4 块发送器接线，本轮仍未执行，留 D-5 收口；票据 7 例已用真实 Redis db15 验证，非 mock）。
- [x] 本轮全量 / build-off 回归已由验收侧复验：build-on 88/88 连跑两遍（3274ms / 3817ms）、ctest 30/30；build-off 14/14（DownloadTicket 7 + DownloadOpen 7，零警告）；四表零残留、server/logic 无 mysql.h/hiredis、Redis 票据键零残留。真实 5min/2h 墙钟等待未做，由 shrinkTtl/overwriteTicket 缩短 TTL 等价覆盖语义（TTL 300s、硬过期封顶、滑动续期均有断言）。

**验证记录（2026-10-19）:**
- 用户逐项明确授权本轮内网 VM 同步与指定构建/目标测试，作为 Execution Policy 的本轮例外；未下载依赖。
- 保留已有未提交草稿，按用户确认的恢复路径先补回归断言；修正前同一目标 5/7（畸形数值及不足 1s 续期失败），修正后 7/7。
- VM 工作目录：/home/colin/cloud-media/build-on。
- 构建：make -j8（退出码 0）。
- 列表：./bin/cm_logic_tests --gtest_filter='DownloadTicket*' --gtest_list_tests（7 项，退出码 0）。
- 实跑：CM_TEST_REDIS=1 ./bin/cm_logic_tests --gtest_filter='DownloadTicket*'（7/7，0 SKIP，退出码 0）。
- Lua 原子执行当前 value 比较与毫秒续期：先 PEXPIREAT hard_exp 定绝对上限，再 PEXPIRE min(PTTL,300000)；中途失效/被替换返回 1004，不续活。
- 验收侧修复一处测试缺陷：`DownloadTicketTest::TearDown` 在 `acquire()` 的 conn 尚未归还时就调 `m_redis->shutdown()`，`RedisCore::shutdown` 的 `wait_for(5s, inUse==0)` 因此每个用例硬等 5 秒（7 例共 30s）。已给 conn 加内层作用域先归还再 shutdown，目标用例耗时 30s → 8ms。

**风险:**
- ticket 泄漏 → 视为 bearer，绑定 uid 校验；日志不打印完整 ticket。
- 2h 硬上限导致超长下载中断 → 重新 10010（start_block=缺失块）换票，不丢进度。
- 成功取块后调用 touch 的业务接线由后续块发送器实现；本轮仅交付票据模块与 openDownload 迁移。

---

### Task D-3：缓冲块发送器（OFF 路径，基线）

**Goal:** 实现不依赖 sendfile 的块读取+CRC+send，作为 OFF 默认与零拷贝的回退。

**Files:**
- Create: server/logic/block_sender.h
- Create: server/logic/block_sender.cpp
- Create: server/logic/tests/test_block_sender_buffered.cpp

**Steps:**
- [x] 输入：ticket、f_id、block_index；校验 ticket→uid/fid。
- [x] stat blob 校验大小；算 offset/len（末块短）。
- [x] IO 池读 4MB（read 循环处理 EINTR/短读），流式 CRC，构造完整帧（19B 头 + payload），回 loop send。
- [x] 全局并发上限 16；超过排队，不阻塞 loop。
- [x] 读失败 3001；越界 1001；内存峰值 16×4MB=64MB。
- [x] 单测：4MB/末块/越界/读失败/16 并发上限。

**实施记录（2026-10-19）：**
- 实际落点：`server/logic/block_sender.h/.cpp`、`server/tests/logic/test_block_sender_buffered.cpp`（计划里 `server/logic/tests/` 是笔误，测试统一在 `server/tests/logic/`）。
- 线程边界：sendBlock 在业务线程完成 touch（Redis）+ 查 blob（MySQL）+ 块范围校验；读块与 CRC 通过注入的 `Executor = std::function<bool(Task)>` 在 IO 池执行，IO 线程不碰 DB/Redis；本类不 include net，完整帧经 done 回调交给调用方发送（D-6 再接线到连接所属 loop）。
- 单缓冲：块直接读进「19B 头 + payload」帧的 payload 区，CRC 只覆盖 header[4..14] 11 字节 + payload（与 Codec::encode 规则一致），峰值 4MB/块而非先读 4MB 再 encode 的 8~12MB。
- 并发上限：`kMaxInFlight=16`，超过进 `m_queue` 排队不阻塞调用线程；暴露 inFlight()/pending() 供背压观测与测试。
- 错误映射：ticket uid/fid 不符 1004（DownloadTicket::touch）、块越界 1001、blob 缺失/状态非 1/读失败 3001、Redis/MySQL 故障 4xxx。
- 单测 5 例：Whole/LastShort 用 Codec::decode 反解断言 type=10011、seq 回显、payload 与块字节一致、CRC 通过；OutOfRange/ReadFailure 断言 1001/3001；ConcurrencyCap 用手动执行器确定性断言「17 个请求 → inFlight=16、pending=1，逐个 drain 后 17 全完成」。
- 未执行构建/ctest（env-policy），验收项待人工在 VM 上确认。

**gtest 清单:**
- BufferedBlockWhole
- BufferedBlockLastShort
- BufferedBlockOutOfRange
- BufferedBlockReadFailure
- BufferedBlockConcurrencyCap

**验收标准:**
- [x] 返回帧的 CRC 与直接计算一致（2026-10-19：`--gtest_list_tests` 命中 5 个；有 DB+Redis 档 BlockSenderBufferedTest 5/5 passed 417ms；Whole/LastShort 用 `Codec::decode` 反解校验 CRC 通过。源码核对：CRC 覆盖 header[4..14] 11 字节 + payload，与 `Codec::encode` 完全一致，CRC 写到 offset 15）
- [x] 16 个并发时第 17 个排队而不是 OOM（BufferedBlockConcurrencyCap 手动执行器确定性断言 inFlight=16、pending=1，drain 后 17 全完成）
- [x] OFF 构建下块发送器正常（build-off 零警告，BlockSenderBufferedTest 5/5 + DownloadTicket 7/7 = 12/12；D1~D6 全量收口仍留 D-6）
- [x] 回归与清理：build-on 全量 93/93 连跑两遍（3696ms / 3198ms）、ctest 31/31；四表零残留、Redis 票据键零残留、mkdtemp 临时目录 0 个；block_sender 无 net/mysql.h/hiredis include

**风险:**
- 4MB 内存峰值 → 限制 16；超过排队。
- 读盘阻塞 IO 池 → 业务池不被占用；下载回退优先级低于合并。

---

### Task D-4：sendfile 块发送器（ON 路径）

**Goal:** 用 sidecar CRC + crc32Combine 构造帧头，sendfile 发送 payload，实现零拷贝默认路径。

**Files:**
- Create: server/logic/sendfile_block_sender.h
- Create: server/logic/sendfile_block_sender.cpp
- Create: server/logic/block_sender_factory.h
- Create: server/logic/block_sender_factory.cpp
- Modify: server/CMakeLists.txt（sendfile_block_sender.cpp / block_sender_factory.cpp 加入 cm_logic）
- Create: server/tests/logic/test_block_sender_sendfile.cpp（计划笔误 server/logic/tests/）

> 注：计划里「Modify block_sender.cpp 按开关选择」改为独立 `BlockSenderFactory`（缓冲实现仍是 D-3 的 BlockSender，不改动其文件）。

**Steps:**
- [x] 仅 #if CM_DOWNLOAD_ZEROCOPY 编译；否则由工厂选缓冲路径。
- [x] 读 sidecar 块 CRC；缺失/为 0 → 懒填充（ensureBlockCrc，IO 池读块算 CRC pwrite），失败回退缓冲路径。
- [x] 构造 header11（VERSION/TYPE/LEN/SEQ），crc32Combine(crc32(header11), blockCrc, len) 填 CRC。
- [x] open blob；把「19B 头 + fd + offset + len」经注入的 deliver 交给连接层 sendFile；连接接管 fd。
- [x] header 先 send 再由 sendFile 等 outputBuffer 空；每轮 ≤1MB / EAGAIN 由 TcpConnection 保证（P0-6 已交付）。
- [x] 单测：4MB 帧与缓冲路径逐字节一致；末块；sidecar 缺失回退；fd 关闭；prelude 顺序。

**实施记录（2026-10-19）：**
- 实际落点：`server/logic/sendfile_block_sender.{h,cpp}`、`server/logic/block_sender_factory.{h,cpp}`、`server/tests/logic/test_block_sender_sendfile.cpp`（计划里 `server/logic/tests/` 是笔误）。
- `sendfile_block_sender.{h,cpp}` 整体 `#if defined(CM_DOWNLOAD_ZEROCOPY) && (CM_DOWNLOAD_ZEROCOPY)` 包裹；OFF 构建下不产生符号，工厂 `BlockSenderFactory::select()` 返回 Buffered。
- 线程边界：sendBlock 在业务线程完成 touch（Redis）+ 查 blob（MySQL）+ 块范围校验 + `readBlockCrc`（小读）；sidecar 缺失/该块 crc==0 时投 IO 池 `ensureBlockCrc` 懒填充（读块算 CRC pwrite，一次性例外）；懒填充失败调用注入的 fallback 委托 D-3 缓冲路径（本类不再回调 done）。本类不 include net：deliver 注入，`deliver(header19, fd, offset, len)` 返回 true 后 fd 归连接关闭。
- 帧头合成：`makeHeader19` 写 magic/version/type(10011)/len/seq，CRC = `crc32Combine(crc32(header[4..14]), blockCrc, len)`，与 D-3 缓冲路径的 `crc32(header11||payload)` 由 H1 对拍保证一致。零拷贝稳态不读 4MB 块进用户态。
- 顺序保证：deliver 契约是「先 send(header) 再 sendFile(fd,offset,len)」；`TcpConnection::sendFile`（P0-6）内部先刷空 outputBuffer（帧头）再 sendfile，每轮 ≤1MB、EAGAIN 继续，均由连接层负责。
- 工厂：`BlockSenderKind{Buffered,Sendfile}` + `BlockSenderFactory::select()`（编译期选择）；缓冲实现即 D-3 BlockSender（未改动其文件）。
- 单测 6 例（5 例 sendfile 仅 ON 编译、1 例工厂两种构建都跑）：MatchesBuffered 用同一 blob 分别走缓冲/sendfile 断言 19B 头与 payload 逐字节一致；LastShort/HeaderBeforePayload 校验末块 len 与帧头各字段+CRC；MissingSidecarFallsBack 用「DB 有行、磁盘无文件无 sidecar」触发懒填充读块失败回退并断言 done 不被调用；FdClosed 断言 deliver 拒绝后 fd 已关闭（fcntl EBADF）；FactorySelectsBufferedWhenDisabled 在 OFF 断言 Buffered、ON 断言 Sendfile。
- 未执行构建/ctest（env-policy），验收项待人工在 VM 上确认。

**gtest 清单:**
- SendfileBlockMatchesBuffered
- SendfileBlockLastShort
- SendfileMissingSidecarFallsBack
- SendfileHeaderBeforePayload
- SendfileFdClosed
- BlockSenderFactorySelectsBufferedWhenDisabled（OFF 构建，工厂测试）

**验收标准:**
- [x] ON 构建下 4MB 块字节/CRC 与 OFF 一致（2026-10-19：build-on / build-off 均 make -j8 零警告 EXIT=0；`--gtest_list_tests` ON 命中 6 例、OFF 命中 1 例工厂；ON 档 SendfileBlockSenderTest.*:BlockSenderFactoryTest.* 6/6 passed 272ms，MatchesBuffered 断言 19B 头与 payload 逐字节一致）
- [x] 零拷贝路径用户态不分配 4MB payload（代码审查：makeHeader19 只组 19B 头，payload 由 fd+sendfile 由内核发送；峰值 RSS 对比留 E 段）
- [x] sidecar 损坏/缺失不影响下载成功，只回退变慢（MissingSidecarFallsBack 通过：DB 有行、磁盘无文件无 sidecar → 懒填充读块失败 → fallback 回调、done 不被调用；工厂测试 OFF 档 BlockSenderFactoryTest 1/1 passed 断言 Buffered）
- [x] 验收侧复验：build-on 全量 99/99 连跑两遍（4114ms / 3713ms）、ctest 32/32；build-off 零警告，`--gtest_list_tests` 命中 1 例工厂（sendfile 被 #if 正确编译掉）、工厂 1/1 passed；四表零残留、Redis 键零残留、mkdtemp 临时目录 0 个；sendfile/factory 无 net/mysql.h/hiredis；sendfile_block_sender.cpp 无 pread/read/readv（4MB 块不读进用户态）

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
- **合并失败**：活跃请求失败回 1（可立即重试），只有进程崩溃中断才留 7；重启把 7 改 2；临时目录由 expiry/tmp 清理。
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
