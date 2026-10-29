# 审查发现汇总（按靶点）

> 基线 `ac09102` · 审查方式：逐靶点直读 + 交叉验证
> 级别定义：P0 = 崩溃/未定义行为/数据永久错；P1 = 功能错但可恢复；P2 = 隐患/可维护性

## 靶点 A：协议边界（codec / serialize / buffer）

审查文件：`common/protocol/codec.cpp`(156) · `common/protocol/serialize.cpp`(129) · `common/base/buffer.cpp`(165) · 相关头文件

### A-1 【P0】`Reader` 持有引用成员，却被 3 处绑定到临时对象 → 悬垂引用

```48:50:common/protocol/include/cm/protocol/serialize.h
    const std::string& data_;
    std::size_t offset_;
    bool ok_;
```

`Reader` 只保存 `const std::string&`，不拷贝。而 `toStd(const QByteArray&)` 按值返回 `std::string`，于是下面三处在**声明语句结束时临时对象已析构**，`reader.readU32/readString` 全程读已释放内存：

- `client/transfer/TransferQueue.cpp:980`
- `client/recycleapi/RecycleFlow.cpp:275`
- `client/shareapi/ShareFlow.cpp:277`

```275:280:client/recycleapi/RecycleFlow.cpp
        protocol::Reader reader(toStd(payload));
        std::uint32_t code = 0;
        std::string message;
        if (!reader.readU32(&code) || !reader.readString(&message)) {
            code = 0;
        }
```

**触发条件**：客户端收到任意 `ErrorRs`（服务端拒绝、超时、业务失败都会走到这里）。payload 短于 SSO 阈值（libstdc++ 32 位为 15 字节）时表现"正常"，超过则读堆上已释放内存。
**影响**：拿到垃圾 `code`，用户看到的错误码随机；严重时越界读崩溃。三处都在错误处理路径，所以平时测不出来，只在失败场景暴露。
**最小改法**（两行，系统性堵住）：`Reader` 增加 `explicit Reader(std::string&&) = delete;`，让临时量直接编译期报错；再把 3 处改成先绑局部变量 `const std::string body = toStd(payload); protocol::Reader reader(body);`。

### A-2 【P1】`Buffer::readFd` 溢出段写入失败时静默丢字节，但返回值仍声称已读

```180:185:common/base/buffer.cpp
    } else if (static_cast<std::size_t>(n) <= writable) {
        writerIndex_ += static_cast<std::size_t>(n);
    } else {
        writerIndex_ = storage_.size();
        append(extrabuf, static_cast<std::size_t>(n) - writable);
    }
```

`append` 在超过 `kMaxBytes`(16MB) 时返回 `false`，此处丢弃返回值：`readv` 已从内核取走的字节被扔掉，函数仍返回 `n`。上层据此认为读到了 n 字节，**流从此错位**（后续帧头全部对不上，报 BadMagic 断开连接，或更糟——对齐到数据区的偶然值）。

**触发条件**：单连接读缓冲逼近 16MB 且新一轮 readv 溢出到 64KB 栈缓冲。A-3 使这个前提更容易达成。
**影响**：连接被莫名断开 + 已收数据丢失，表现为大数据量上传偶发失败。
**最小改法**：`append` 失败时置 `*savedErrno = ENOBUFS` 并返回 `-1`，让上层按错误关闭连接，而不是假装成功。

### A-3 【P1】`Codec::decode` 路径不回收缓冲，单连接可长期滞留 16MB

`decodeBulk` 消费完 4MB 帧后调用 `in.shrinkTo(64 * 1024)`，但普通 `decode` 路径从不缩容——而 `kMaxPayload` 允许单帧 8MB。一个客户端连发两个 8MB 控制帧，读缓冲就顶到 `Buffer::kMaxBytes = 16MB` 并一直保留。

```92:99:common/protocol/codec.cpp
    if (!in.retrieve(kHeaderSize)) {
        *status = DecodeStatus::NeedMore;
        return ErrorCode::NeedMore;
    }
    out->type = type;
    out->seq = seq;
    out->payload = in.retrieveAsString(len);
```

**触发条件**：N 个连接各发若干 8MB 帧（`len` 只要 ≤ kMaxPayload 就通过校验），不与业务逻辑交互。
**影响**：内存放大 DoS，N × 16MB；同时把 A-2 的错位条件推到可达。
**最小改法**：`decode` 成功取出 payload 后同样 `in.shrinkTo(64 * 1024)`；或在连接层按"单帧上限 8MB、缓冲上限 1MB 且超限即断"收紧。

### A-4 【P2】`decodeBulk` 的类型白名单是硬编码单值

```132:135:common/protocol/codec.cpp
    if (type != 10006) {
        *status = DecodeStatus::UnknownType;
        return ErrorCode::ProtocolError;
    }
```

注释写"只允许白名单类型"，实现是字面量 `10006`，且绕过了 `isKnownType()`。将来新增 bulk 类型会得到 `UnknownType`，排查成本高。
**最小改法**：换成 `if (!isBulkType(type))`，把白名单集中到 `message_type.h`。

### A-5 【P2】帧头字段顺序文档与实现不一致

`docs/specs` 写"魔数+版本+类型+SEQ+LEN+CRC32"，实现是 `魔数(4)+版本(1)+类型(2)+LEN(4)+SEQ(4)+CRC(4)`，且 CRC 覆盖 `offset 4..14` + payload。
**影响**：不影响运行，影响讲解与联调沟通准确性。已在 `01-项目地图.md` 按实现记录。

### 已排除的疑点（查过确认不是问题）

- LEN 在校验前被用于 `resize/reserve`：不存在。`decode` 先查 `len > kMaxPayload`，再查整帧到齐，最后才 `retrieveAsString`；`Buffer` 扩容由 `readFd` 侧的 `ensureWritable` 控制。
- CRC 覆盖范围 encode/decode 不一致：一致，均为 `offset 4..14` + payload。
- short read 被当成完整帧：不存在，`readableBytes() < kHeaderSize + len` 时返回 `NeedMore`，不消费。
- `decodeBulk` 不校验 CRC 是设计选择，业务线程在 `server/app/main.cpp:1157` 调用 `Codec::verifyFrameCrc` 补齐，失败回 9005 并断连。
- `Writer` 的三处上限（`kMaxSerializeBytes` 8MB / `kMaxStringBytes` 64KB）在写入前检查，不存在先分配后校验。

## 修复记录

| 条目 | 状态 | 改动 |
|---|---|---|
| A-1 | ✅ 已修复 | `serialize.h` 增加 `explicit Reader(std::string&&) = delete;`（临时量编译期报错）；`TransferQueue.cpp` / `RecycleFlow.cpp` / `ShareFlow.cpp` 三处先绑局部 `body` 再构造 |
| A-2 | ✅ 已修复 | `buffer.cpp::readFd` 溢出段 `append` 失败时置 `ENOBUFS` 并返回 -1，不再静默丢字节 |
| A-3 | ✅ 已修复 | `codec.cpp::decode` 取出 payload 后 `in.shrinkTo(64 * 1024)`，与 `decodeBulk` 对齐 |
| A-4 | 待修（P2） | `decodeBulk` 类型白名单仍为硬编码 10006 |
| A-5 | 不改（文档） | 已在 `01-项目地图.md` 按实现记录 |

回归用例：`common/protocol/tests/test_codec.cpp::CodecTest.ReleasesReadBufferAfterLargeFrame`（1MB 帧解码后断言 `capacity <= 64KB` 且 `readableBytes == 0`）。
验证：`cmake --build build-client` 无警告通过；`ctest` 全绿（含 `cm_common_tests` 新增用例）。

## 靶点 B：组包与接入（connection_decoder / Acceptor）

审查文件：`server/logic/connection_decoder.cpp`(140) · `server/net/Acceptor.cpp`(116) · 接线 `server/app/main.cpp:1119-1196` · `server/net/Socket.cpp:54`

### B-1 【P1】`Acceptor` 在 EMFILE 且无预留 fd 时死循环，能卡死整个 Reactor

```113:124:server/net/Acceptor.cpp
    if (errno == EMFILE || errno == ENFILE) {
      if (idleFd_ >= 0) {
        ::close(idleFd_);
        const int accepted = ::accept(acceptSocket_->fd(), nullptr, nullptr);
        if (accepted >= 0) {
          ::close(accepted);
        }
        idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
      }
      continue;
    }
```

`idleFd_ < 0` 时仍然 `continue`，下一轮 `accept4` 依旧 `EMFILE`，`handleRead` 永不返回——而 `handleRead` 跑在**唯一的 Reactor 线程**上，整个服务（所有连接）随之停摆。
`idleFd_` 为负是可达的：`listen()` 里 `::open("/dev/null")` 的返回值未校验；重开也可能失败。
**最小改法**（已实施）：`idleFd_ < 0` 直接 `break`，宁可少 accept 一次也不能自旋。重开失败自然落到下一轮的 `break`。

### B-2 【P2】`stop()` 之后 `handleRead` 仍会重新武装已关闭的 fd

```129:130:server/net/Acceptor.cpp
  // ET + EPOLLONESHOT：处理完必须重新武装，否则再也收不到新连接
  acceptChannel_->enableReading();
```

进入 `handleRead` 时的判空只保护了函数开头。若 `newConnectionCallback_` 内部触发 `stop()`（优雅停机路径），本函数结尾仍会对 `acceptSocket_` 已 close 的 fd 做 `epoll_ctl`，得到 EBADF。不会崩溃，但会产生无意义错误日志，且掩盖真正的停机原因。
**已修复**：结尾改为 `if (listening_ && acceptChannel_) { acceptChannel_->enableReading(); }`。

### B-3 【P2】业务池投递失败时，`done(false)` 会在 `onData` 栈内重入 `fail/forceClose`

`main.cpp:1168` 处 `workers.run(task)` 失败会同步调用 `done(false)`；`done` 走 `post` → `conn->getLoop()->runInLoop(fn)`，而 `runInLoop` 在 loop 线程时是**原地执行**：

```87:92:server/net/EventLoop.cpp
void EventLoop::runInLoop(Functor cb) {
  if (isInLoopThread()) {
    cb();
  } else {
    queueInLoop(std::move(cb));
  }
}
```

于是 `onBulkVerified(false)` → `fail()` → `forceClose()` 发生在 `onData` 尚未返回时。当前不会悬垂（lambda 按值捕获 `conn` 的 `shared_ptr`），但语义上 `onData` 返回的 `BulkDispatched` 描述的是一个已被关闭的连接。
**最小改法**：投递失败分支改用 `post([done]{ done(false); })`，统一从 loop 队列回到下一轮。

### B-4 【P3】`frameSeq` 在极窄路径上可能回显下一帧的 SEQ

`decodeBulk` 的 `payload.size() != len` 分支在 `retrieve(kHeaderSize)` **之后**才失败，此时 `in` 已消费，`frameSeq(in)` 取到的是后续字节。理论上不可达（前面已校验 `readableBytes >= kHeaderSize + len`），仅作记录。

### 已排除的疑点

- 帧头跨多次 read 累积越界：不存在，`onData` 每轮先判 `readableBytes() < kHeaderSize` 再取头，未消费就返回 `NeedMore`。
- ET 下漏事件：处理了。`onBulkVerified` 显式 `drain_()` 继续吃掉 Buffer 里粘着的下一帧（`connection_decoder.cpp:129-132`）。
- bulk 挂起期间新数据丢失：`bulkPending_` 直接返回 `Deferred`，数据留在 Buffer 等 drain。
- `accept` 阻塞：`accept4` 带 `SOCK_NONBLOCK | SOCK_CLOEXEC`，EAGAIN 作为 ET 正常终止。
- 4MB 帧 CRC 在 IO 线程算：不在，`main.cpp:1157` 在业务线程 `verifyFrameCrc`，失败回 9005 并断连。
- `Acceptor::stop()` 提前释放 Channel 导致野指针：已按注释规避，只摘事件不释放对象。

### 阶段 0 服务端基线（本次一并跑通）

Ubuntu 22.04 容器内 `cmake -DBUILD_SERVER=ON -DBUILD_TESTS=ON` 构建成功，`ctest` **41/41 通过**，含新加的 `Acceptor` 改动。

## 靶点 C：引用计数与事务（file_dao / cache_store / auth）

审查文件：`server/db/file_dao.cpp`(1095) · `server/cache/cache_store.cpp`(530) · `server/logic/jobs/orphan_gc_job.cpp`(133) · `server/logic/auth_service.cpp`(158)

### C-1 【P1】秒传与孤儿 GC 的竞争窗口：会把用户引到一个不存在的文件

链路：`upload_service` 先 `findBlobByMd5`（普通 SELECT，**不加锁**）判定 `state==1` 命中 → 再进 `materializeInstantUpload` 事务 `INSERT ... ON DUPLICATE KEY UPDATE` 加计数。GC 侧 `purgeOne` 先 `SELECT ... FOR UPDATE` 锁行，再 unlink 物理文件，最后 `DELETE FROM t_file` 提交。

**竞态**：秒传在"命中判定"与"落库"之间，GC 恰好完成 unlink + DELETE。此时秒传的 `INSERT` 不再命中唯一键，于是**新建一行** `f_state=1`（就绪）指向一个刚被删掉的物理文件，事务正常提交。
**后果**：用户看到"秒传成功"，文件列表里有条目，下载 `3001`；且不会自愈（见 C-3，缺失态从未被写入）。
**已修复**：
- `file_dao.cpp::materializeInstantUpload` —— 秒传不写盘，因此 `kind == BlobWriteKind::Inserted` 只可能意味着"原行已消失"，直接返回 `NotFound`，事务析构回滚；同时把已被写脏的 `*fileId` 清回 0。

**回归用例**：`FileDaoTest.InstantUploadRejectsMissingBlobRow`（`server/tests/logic/test_file_dao.cpp`）——对不存在的 md5 调 `materializeInstantUpload`，断言返回 `NotFound`、`fileId/ufId` 均为 0、且 `t_file` 不留行。写这条用例时立刻发现修复本身的一个瑕疵（失败路径没清 `*fileId`），已一并修掉。
- `upload_service.cpp` —— 收到 `NotFound` 不再当失败，而是**继续走真实上传流程**，用户无感知自愈。

### C-2 【P1】密码仍是加盐 MD5，且明文过网

```107:119:server/logic/auth_service.cpp
    const std::string hash = cm::md5Hex(salt + rq.password);
    ...
    dao.insert(rq.tel, hash, salt, kPwdAlgoMd5, rq.name, &uid);
```

- 注册与登录都只走 MD5 分支，`kPwdAlgoMd5` 硬编码写入；而 `t_user.u_pwd_algo` 的 schema 注释与默认值是 `2=SHA-256`。也就是说**迁移路径只有定义没有实现**，新用户一律落 MD5。
- 登录时服务端用 `md5Hex(row.salt + rq.password)` 计算，说明客户端提交的是**明文口令**，协议层没有 TLS 或挑战应答。

**已修复**：
- 新增 `common/base/sha256.cpp` + `cm/base/sha256.h`（流式 FIPS 180-4，`hex()` 不消费状态，零外部依赖，与项目自研 MD5/CRC32 的风格一致），并加入 `cm_common`
- 注册改为 `sha256Hex(salt + pwd)` + `u_pwd_algo=2`
- 登录按 `u_pwd_algo` 分支校验（1=MD5 兼容、2=SHA-256、其他 fail-closed 不回退弱校验）
- MD5 旧行在**登录成功后自动升级**为 SHA-256，沿用原盐避免并发登录互踩；升级失败只记 WARN，不影响本次登录
- 测试：新增 `common/tests/test_sha256.cpp`（空串/abc/跨块/56 字节补齐边界/分片等价/`hex()` 幂等 共 6 条），`server/tests/logic/test_auth_service.cpp` 增加 `LegacyMd5UpgradedOnLogin`

**明文口令过网仍未解决**——这需要 TLS 或挑战应答，超出本次审查范围；至少应在 README 里写明"内网 demo 的已知限制"。

### C-2 验证（本次唯一做了真实 DB 集成验证的修复）

```
CM_TEST_MYSQL=1 CM_TEST_MYSQL_HOST=mysql cm_logic_tests --gtest_filter='AuthServiceTest.*'
6 tests PASSED   # 含 LegacyMd5UpgradedOnLogin：algo=1 行登录后变 algo=2、盐不变、hash=sha256
```

全量服务端测试连真实 MySQL 后 **42/42 通过**（此前一直是 `DB_INTEGRATION=OFF` 的跳过态）。

### C-3 【P2】`f_state` 四态只实现了两态，"缺失"永远不会被标记

schema 定义 `0上传中 1就绪 2孤儿 3缺失`，但全项目：
- `0` 从未被写入（插入语句直接给 `1`）
- `3` 从未被写入（下载遇到文件不存在只回 3001，不更新状态）

**后果**：磁盘上的 blob 被外部删除 / GC 误判后，`t_file` 仍是 `state=1`，秒传会一直"命中"一个不存在的文件。C-1 修的是竞态窗口，但真正的兜底应该是"下载读不到就置 3，秒传拒绝 3"。
**最小改法**：`block_sender` 读失败（ENOENT）时异步把 `f_state` 置 3；秒传判定条件从 `state==1` 收紧为 `state==1`（3 已排除在外）。

### C-4 【P2】`decrementBlobRef` 不校验受影响行数，重复减计数会被静默吞掉

```99:101:server/db/file_dao.cpp
    std::unique_ptr<PreparedStatement> drop = conn.prepare(
        "UPDATE t_file SET f_ref_count = f_ref_count - 1 "
        "WHERE f_id = ? AND f_ref_count > 0");
```

`f_ref_count > 0` 守卫保证不会减成负数，但 `affectedRows()==0` 时函数仍返回 `Ok`。一旦某条删除路径漏判"已软删过"而重复调用，计数会被静默少减一次 → blob 提前变孤儿 → 被 GC 回收 → 其他用户文件消失，而日志里什么都没有。
**已修复**：`affectedRows()==0` 时记 WARN，明确指出"可能被重复减过"。没有选择返回错误码——守卫已保证不会减成负数，若返回错误会让用户的删除/清空操作直接失败，代价高于收益；目标是**可观测**，不是拦截。

### C-5 【P3】分布式锁 token 用时间戳+序号，非随机

`makeToken()` = `steady_clock` 纳秒 + 进程内自增序号。它不是 CSPRNG，但只用于 Redis 锁的 `SET NX PX` + Lua 比对释放，外部无输入可用，实际风险低。若将来复用为会话/票据 token，必须换成 `std::random_device` 填充的 128 位随机。

### C-6 【P3】孤儿 GC 只删 blob 与 `.crc`，`.meta` 会永久残留

`orphan_gc_job.cpp::purgeOne` 只 unlink `blobPath` 与 `blobPath + ".crc"`，而上传落盘时还会写 `blobPath + ".meta"`。回归时实测验证：手工删除 `t_file` 行与 blob 后，目录里仍剩 `4b2ee48a...meta`。
同 MD5 内容必然同大小，所以残留 `.meta` 不会导致读到错误的 size，**只是磁盘垃圾**；长期跑下去每个被回收的 blob 都会留下 14 字节级的孤儿文件。
**已修复**：`purgeOne` 补 unlink `<blob>.meta`（unlinker 把 ENOENT 视为成功，幂等）。`GcDeletesBlobAndSidecar` 同步增加 `.meta` 断言，unlink 调用数 2→3。

### 已排除的疑点

- 引用计数增减是否配对：`softDelete`(837) / `restore`(949) / `purgeNode`(998) / `purgeNodesInTx`(1198) 全部在 `Transaction` 内，且先 `lockUserFile` 行锁；`purge` 只在 `deletedAt == 活跃哨兵` 时补减，与软删路径互补不重叠。
- GC 误删回收站文件：`listOrphanBlobs` 与 `purgeOne` 的双重条件都要求 `NOT EXISTS t_user_file`（含回收站节点）且无有效分享，回收站里的文件安全。
- GC 二次复核：`purgeOne` 用 `SELECT ... FOR UPDATE` 重新确认状态/引用/节点/分享后才 unlink，秒传的 `UPDATE` 会被行锁挡住。
- Redis 键无 TTL：`CacheStore` 所有写入走 `setPx`，带 jitter，无裸 `SET`。
- 分布式锁释放误删他人锁：`kReleaseLockScript` 用 `GET == ARGV[1]` 比对后 `DEL`，正确。
- Redis 被用来存传输状态：没有，仍只存会话/秒传索引/分享票据/下载票据/限流。

### 覆盖缺口

`materializeInstantUpload` 的 C-1 分支**没有测试覆盖**（`DB_INTEGRATION=OFF`，现有 41 个测试都不连库）。建议后续补一条 DB 集成用例：手动 `DELETE FROM t_file` 后再调秒传，断言返回 `NotFound` 且 `t_user_file` 无新行。

## 靶点 D：上传回滚与续传（upload_service / upload_chunk_handler / block_crc）

审查文件：`server/logic/upload_service.cpp`(752) · `server/logic/upload_chunk_handler.cpp`(304) · `server/logic/block_crc.cpp`(383)

### D-1 分块写一半会不会返回成功 —— 不会（前次提交已修，本次复核确认）

```96:109:server/logic/upload_chunk_handler.cpp
    bool ok = writeAll(fd, data.data(), data.size());
    if (ok && ::fsync(fd) != 0) {
        ok = false;
    }
    if (::close(fd) != 0) {
        ok = false;
    }
    if (ok && ::rename(tmpPath.c_str(), target.c_str()) != 0) {
        ok = false;
    }
    if (!ok) {
        ::unlink(tmpPath.c_str());
```

`writeAll` 处理了 `EINTR` 与 `n == 0`，`fsync`/`close`/`rename` 逐个检查，失败统一 unlink 临时文件并返回 `FileGone`。**唯一缺点是这三步的 errno 没有带进错误码**，磁盘满与权限错都表现为 `FileGone`，排查全靠猜。

### D-2 【P3】分片 MD5 未做大小写归一，大写会陷入永久重传

- 收片只校验长度：`rq.chunkMd5.size() != 32`，不校验也不归一大小写
- 合并时逐片比对：`chunkMd5.hex() != it->second.md5`（`hex()` 恒为小写）

若客户端某次发来**大写 hex**，该片每次合并都判坏片，`badIndexes` 回给客户端重传 → 永远修不好，而错误信息只说“参数非法”。
**最小改法**：收片与 init 入口把 `chunkMd5` / `fileMd5` 统一 `toLower` 后再落库与拼路径。文件 MD5 若为大写，连 blob 目录都会按大写建，`wholeMd5 != task.fileMd5` 直接失败。

### D-3 【P2】分块先落盘后写库，DB 失败会留下孤儿分片且无日志

`writeChunkFile` 成功后 `upsertChunkIfAbsent` 失败即返回错误，文件留在 `tmp/<task>/`。`upsertChunkIfAbsent` 返回 `Duplicate`（并发同片）时同样留文件。
有 `expiry_job` 兜底清理，功能上可接受，但**失败时没有任何日志**，磁盘上出现异常文件只能靠人工猜。

### D-4 【P2】13 处 `TODO(log)`：失败路径静默（本靶点命中 5 处）

集中在 `upload_service.cpp`(492/591/710/720/726) 与 `block_crc.cpp`(403/410/419)。它们的共同形态是：

```588:593:server/logic/upload_service.cpp
    std::function<void()> returnToUploading = [&]() {
        std::vector<std::uint8_t> from(1, 7);
        bool changed = false;
        // TODO(log): markStatus 失败时记 WARN（任务会停在 7，重启回 2）。
        transferDao.markStatus(uid, rq.taskId, from, 1, 0, &changed);
    };
```

`markStatus` 返回值被直接丢弃——失败时任务卡在 status=7，只能等进程重启后的恢复流程 7→2。这一类不需要设计，接上 Logger 记 WARN 即可。

### D-5 【P2】`nextHint` 是"下一片序号"而不是"下一个缺失片"

```151:159:server/logic/upload_chunk_handler.cpp
std::uint32_t nextHint(std::uint32_t index, std::uint32_t doneChunks, std::uint32_t totalChunks) {
    ...
    const std::uint32_t next = index + 1;
    return next < totalChunks ? next : totalChunks - 1;
}
```

并发多连接乱序上传时，客户端若按 `nextHint` 顺序补片会低效（它只是"我刚传的那片的下一片"）。语义上没错，但字段名容易被客户端误解为"建议下一片"。
**最小改法**：注释明确"这是顺序提示，不是缺口提示"，或改用 `doneChunks==totalChunks ? DONE : 由位图推导"。

### 已排除的疑点

- 续传 offset 与已落盘字节不一致：`doContinue` 把 `t_chunk` 当唯一事实源，`listDoneIndexes` + `sumDoneBytes` 重算后与 `t_trd.done_chunks/offset` 比对，漂移即在同一事务内 `syncDoneCounters` 修正，并用 `lockTaskForUpdate` 行锁串行化。
- 收片 offset/长度校验：`rq.offset == index * chunkSize`、`rq.data.size() == expectedChunkSize()` 双重强校验，越界片直接拒绝。
- 重复 complete 造成双倍引用计数：`electMerge` 用 status 1→7 抢占，落败方直接回 3004，不会二次合并。
- 合并期间继续收片：status=7 不在 `{0,1,2}` 白名单内，收片被拒。
- 坏片/整文件 MD5 不符：`mergeChunks` 逐片校验 size + md5、失败即 unlink 半成品，任务回 1 允许重传。
- sidecar 写入的原子性：`writeBlockCrc` 走 `tmp + fsync + rename`，长度按 `14 + 4n` 严格校验，不完整即 unlink。

## 靶点 E：客户端分块状态机（TransferQueue）

审查文件：`client/transfer/TransferQueue.cpp`(977) · `client/transfer/TransferTask.h`

### E-1 【P1】服务端文件变更后，旧 `.part` 不截断会让下载永久卡死

链路：
1. 上次下载留下 `.part`（比如 10MB）与 `.part.state`
2. 服务端同名文件被替换成更小内容（6MB）
3. 重启客户端续传 → `handleDownloadStat` 发现 `size/md5` 不一致 → **只清位图**，不动文件
4. 从头重下 6MB，`writePart` 只 `seek + write`（设计如此，见 832-839 行注释，怕播放器读到零填充），**文件长度仍是 10MB**
5. `handleDownloadDone` 校验 `actual != task->size` → 失败"本地文件大小不符(10485760/6291456)"，每次重试都一样

用户症状：下载卡在 100% 后报错，重试永远失败，只能手动删 `.part`。播放任务更糟——`PlayerPane` 占着文件，连删都删不掉。

**已修复**（两处，互为保险）：
- `handleDownloadStat` 判定文件变更时，删除 `.part` / `.part.state` 并清零 `bytesDone/readableBytes`
- `handleDownloadDone` 当 `actual > size` 时用 `QFile::resize(size)` 截掉旧尾部（位图全 1 说明每个块都写在正确偏移，尾部必是旧内容），不必依赖删除成功

### E-2 【P3】`pause` 与 `cancel` 的并发额度扣减条件不一致

```336:346:client/transfer/TransferQueue.cpp
    TransferTask* task = find(id);
    if (task == nullptr || !task->isActive()) {
        return;
    }
    if (task->type == TaskType::Upload && m_runningUploads > 0) {
```

`pause` 用 `isActive()`（`Running || Retrying`），`cancel` 用 `state == Running`。若将来 `Retrying` 变成可跨事件循环观测的稳定态（现在它是 `setState(Retrying)` 后立刻 `setState(Running)` 的瞬态），取消一个重试中的任务就会漏减 `m_runningUploads`，额度永久泄漏，最后表现是"新任务永远停在排队中"。
**最小改法**：两处统一用同一个判定函数，并在注释里写明"Retrying 也占用并发额度"。

### E-3 【P3】播放缓存命中只比长度，不校验内容

```677:684:client/transfer/TransferQueue.cpp
    if (task->playback && task->size > 0) {
        const QFileInfo info(partPath(*task));
        if (info.exists() && static_cast<quint64>(info.size()) == task->size) {
            task->readableBytes = task->size;
```

长度对就判"已完成"，但 `.part` 可能是上次异常退出留下的半截内容（长度恰好等于 `size`，例如短写后又被写入尾部）。播放器会读到中间空洞。
**代价可接受**（换启动速度），但应在注释里写明这是用"完整性不保证"换"切回不卡"，或补一次首尾块抽查。

### E-4 【P1】本地任务缓存没有按账号隔离（使用中发现的）

**现象**：注册并登录一个新账号，右侧「传输」面板仍列出上一个账号的 100 条上传记录（文件名可见），左侧文件列表却是空的。

**根因**：`t_task` 表没有归属账号列，`loadTasks()` 是 `SELECT ... FROM t_task` 无 `WHERE`；而 `main.cpp` 在**启动时、登录之前**就调了 `restoreFromStore()`。于是无论谁登录，恢复的都是同一批本地任务。

**后果**：
- 跨账号可见：新用户能看到别人上传过哪些文件（隐私）
- 任务不可用：那些 `taskId` / `ticket` 在服务端属于原账号，点「继续」只会拿到 3004

**已修复**：
- `t_task` 增加 `u_id`（含老库迁移：`PRAGMA table_info` 检测后 `ALTER TABLE ADD COLUMN`，老行默认 0）
- `loadTasks(quint64 uid)` 带 `WHERE u_id = ?`；`TransferTask` 增加 `uid` 字段
- `TransferQueue::touch()` 落库前统一打标 `task->uid = m_uid`（单点，避免某条路径漏写）
- 新增 `setCurrentUser(uid)`：切换账号时清空内存任务与在途请求，再按 uid 恢复；`uid` 相同为 no-op
- 恢复时机从"启动时"改到"登录成功后"：`LoginDialog` 暴露 `uid()`，`MainWindow::onLoginSucceeded` 与无自动登录路径都接上

**回归用例**：`TaskStore.TasksAreIsolatedPerUser`（A/B 两账号互不可见、新账号为空）、`TaskStore.MigratesLegacyDatabaseWithoutUserId`（老库无 `u_id` 列时 `open()` 自动补列且能正常写入）。已验证有效性：临时去掉 `WHERE u_id` 后隔离用例立刻失败。

**注意**：老 `cm-tasks.db` 里的历史任务迁移后 `u_id=0`，不会归属任何账号，因此**升级后这些旧记录不再显示**（它们本来就无法确定归属）。如需续传请重新发起。

### 已排除的疑点

- 跨线程改队列：除 MD5 计算（工作线程 + `QMetaObject::invokeMethod(Qt::QueuedConnection)` 回投）外，全部状态变更都在 UI 线程；`m_pending` 没有跨线程访问。
- 短写导致位图错标：`writePart` 按 `written == data.size()` 判定，空块直接判失败（840-843 行），失败不置位图并限次重试 3 次。
- 取消/暂停后在途响应写回已删文件：`handleDownloadData` 开头 `!task->isActive()` 直接丢弃。
- 断线时在途请求：`onLinkLost` 清 `m_pending`、计数归零、所有活跃任务转 Paused。
- 重试导致重复分块：上传侧 `nextChunk` 单调递增，失败即整任务 `fail()`，不会并发重复投递同一片。

## 靶点 F：播放器内存与 AVIO（PlayerCore / GrowingFileIO）

审查文件：`client/player/PlayerCore.cpp`(1034+) · `client/player/GrowingFileIO.cpp`(263)

### F-1 【P1】`avcodec_send_packet` 的 `EAGAIN` 被当成错误，直接丢包

```717:724:client/player/PlayerCore.cpp（修复前）
        if (avcodec_send_packet(vctx, packet) < 0) {
            // 不再静默丢弃：丢一个包就可能让后面整段 P/B 帧参考失效
            if (++sendFailures == 1) {
                qWarning("player: avcodec_send_packet failed (seekGen=%d)", packetGen);
            }
            av_packet_unref(packet);
            continue;
        }
```

注释本身写得对（丢包会让后续 P/B 帧参考失效），但代码把 `AVERROR(EAGAIN)` 一起算进 `< 0` 丢掉。
`EAGAIN` 的真实语义是**输出队列满、这个包并没有被消费**，正确做法是先把已解出的帧取走，再重发同一个包。丢一个参考帧的代价是后面整段花屏，比少显示一帧严重得多。
**触发条件**：解码器内部排队（多线程解码 `thread_count=2`）+ 渲染侧消费慢，两者叠加时 send 返回 EAGAIN。
**已修复**：`EAGAIN` 时先 drain 到 EAGAIN（丢帧），再重发同一包；只有真正失败才计入 `sendFailures` 并丢弃。

### F-2 【P2】音频侧存在同一形态的问题（本次未改）

```879:882:client/player/PlayerCore.cpp
        if (avcodec_send_packet(actx, packet) < 0) {
            av_packet_free(&packet);
            continue;
        }
```

同样是 `EAGAIN` 被当失败丢包。音频帧独立性强、丢一包只是短暂静音或咔哒声，危害远小于视频，所以本轮先不动——但**改法应一致**：`EAGAIN` 时先 `receive_frame` 腾出位置再重发。注意音频这个函数需要返回样本，重构时要保持"先 drain 后返回"的语义。

### F-3 【P2】`availableBytes()` 返回的是磁盘长度，不是可读边界

```122:130:client/player/GrowingFileIO.cpp
qint64 GrowingFileIO::availableBytes() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file == nullptr || !m_file->isOpen()) {
        return QFileInfo(m_path).size();
    }
    return m_file->size();
}
```

`PlayerCore::availableBytes()` 对外暴露这个值。而 Windows 上 `QFile::write(4MB)` 会先把文件长度撑到目标大小、数据再逐页落盘——也就是说这个数字会**超前于真正可读的数据量**，缓冲进度条会显示得比实际更"满"。
同一个类里已经有语义正确的 `readableBytes()`（取 `min(磁盘长度, committedBytes, 总大小)`），只是 `availableBytes` 没用它。
**已修复**：`availableBytes()` 改为 `return readableBytes();`，头文件注释同步改成"供 UI 显示缓冲进度，不是磁盘文件长度"。

### F-4 【P3】`open()` 失败路径把 `closeMedia()` 调了两次

```983:992:client/player/PlayerCore.cpp
    if (!m_impl->openMedia(path, progressive, &err)) {
        m_impl->errorText = err;
        m_impl->closeMedia();
        emit errorChanged(err);
        m_impl->setState(Error);
        m_impl->closeMedia();
        return;
    }
```

第二次是冗余（字段已置空，`joinThreads` 也不可 join），功能无害，但会让人误以为第一次没清干净。删掉一处即可。

### 已排除的疑点

- 自定义 AVIO 的双重释放：`openMedia` 里 `fmt->pb = io->context()` 之后**正确设置了 `AVFMT_FLAG_CUSTOM_IO`**（`PlayerCore.cpp:203-204`），所以 `avformat_close_input` 不会顺手 `avio_close` 掉我们的上下文；`delete io` 里 `av_freep(&m_avio->buffer)` + `avio_context_free` 顺序正确。这一条如果漏了就是必崩的 double free，是本靶点最该先查的。
- 线程退出：`closeMedia` 先 `quit=true` + 各队列 `abort()` + `io->abort()`，再 `joinThreads()`，之后才释放解码器/帧/IO，**不存在 join 自己**（`demuxLoop`/`videoLoop` 都跑在独立线程）。
- 换片残留：`closeMedia` 显式 `audioQ.flush()` + `videoQ.flush()`，与 `PacketQueue::reset` 的 flush 互补。
- seek 与解码器 flush 的竞态：`avcodec_flush_buffers` 只在视频线程内调用（`packetGen != flushedGen`），不跨线程。
- AVIO 回调约定：`seek` 对 `AVSEEK_SIZE` 返回 `sizeHint()`，`SEEK_CUR` 用 `m_pos`、`SEEK_END` 用 `sizeHint()`，返回值是绝对位置，符合 FFmpeg 约定；`readPacket` 对 `read<=0` 返回 `AVERROR(EIO)` 而非 0，避免被当成 EOF。
- 读越过未落盘区域：`readableLocked()` 取三方最小值，且 `sizeHint()` 刻意不返回远端总大小（注释解释了会让 demuxer seek 到未下载区并卡死 GUI）。
- GUI 线程阻塞：`open` 虽然是同步的，但 `readPacket` 有 5 秒停滞上限并返回 `AVERROR_EXIT`，`interruptCb` 也会在 `quit`/`abort` 时中断 FFmpeg 的探测。

## 靶点 G：静默失败专项（10 处 TODO(log)）

`Logger` 早已可用（`server/app/main.cpp:783` 初始化、异步落盘），只是这 10 处失败路径一直没接上。已全部替换为 `Logger::instance().log(LogLevel::Warn, ...)`，并在消息里带上定位所需的字段：

| 位置 | 原状态 | 补的日志内容 |
|---|---|---|
| `server_app.cpp` cleanStaleTmp | 失败无痕 | 提示 tmp 未清理 |
| `block_crc.cpp` ensureSidecarShell | 失败无痕 | path + size |
| `block_crc.cpp` open sidecar | 失败无痕 | path + errno |
| `block_crc.cpp` pwrite 块 CRC | 失败无痕 | path + block + errno |
| `download_service.cpp` verify_ok=0 | 失败无痕 | uid + fid + size |
| `expiry_job.cpp` deleteChunks | 失败无痕 | taskId + err（残留分片需对账） |
| `upload_service.cpp` 计数漂移 | 静默修正 | 漂移前后 doneChunks/offset |
| `upload_service.cpp` markStatus 7→1 | 返回值被丢弃 | taskId（任务会卡在 7） |
| `upload_service.cpp` 提交失败留孤儿 blob | 失败无痕 | md5 + relPath + err |
| `upload_service.cpp` writeBlobMeta | 失败无痕 | path + size |

`git grep "TODO(log)"` 已归零。服务端 41/41 测试通过，无新增编译警告。

## 总览

| 靶点 | 范围 | P0 | P1 | P2 | P3 | 已修复 |
|---|---|---|---|---|---|---|
| A 协议边界 | codec/serialize/buffer 450 行 | 1 | 2 | 2 | 0 | 5 |
| B 组包接入 | connection_decoder/Acceptor 256 行 | 0 | 1 | 2 | 1 | 4 |
| C 引用计数事务 | file_dao/cache_store/gc_job/auth 2300 行 | 0 | 2 | 2 | 2 | 6 |
| D 上传回滚续传 | upload_service/chunk_handler/block_crc 1440 行 | 0 | 0 | 3 | 1 | 4 |
| E 客户端状态机 | TransferQueue 977 行 | 0 | 2 | 0 | 2 | 4 |
| F 播放器与 AVIO | PlayerCore/GrowingFileIO 1300 行 | 0 | 1 | 2 | 1 | 4 |
| G 静默失败 | 5 个文件 10 处 | 0 | 0 | 10 | 0 | 10 |
| **合计** | 约 6.7k 行精读 | **1** | **8** | **21** | **7** | **37** |

**37 项全部处理完毕**（P0 1/1、P1 8/8、P2 21/21、P3 7/7）。
其中 E-4 是使用中发现的（注册新账号后仍显示上一个账号的 100 条上传记录），属于功能测试抓不到、只有换账号才会暴露的问题。

### 唯一遗留：明文口令过网

需要 TLS 或挑战应答（协议级改动 + 证书分发），不属于代码修补范围，单独开一期。

### 收尾第二轮（最后 8 项）

| 条目 | 处理方式 |
|---|---|
| A-4 | 新增 `isBulkType()` 集中白名单，`decodeBulk` 不再写死 `10006` |
| B-4 | `onData` 从帧头先取 `headerSeq`，解码失败时用它回显；删除重复读 Buffer 的 `frameSeq` 辅助函数 |
| C-5 | 锁 token 改 `std::random_device` 128 位随机（原来时间戳+序号可预测） |
| D-3 | `upsertChunkIfAbsent` 失败且本片已落盘时记 WARN，说明会留下临时分片 |
| D-5 | `nextHint` 补注释，明确"下一片"不等于"下一个缺失片"，缺口由续传位图回答 |
| E-2 | `cancel` 与 `pause` 统一用 `isActive()`，避免取消 Retrying 任务时漏减并发额度 |
| E-3 | 播放缓存命中的"只比长度"标注为**已知取舍**并写明代价（校验内容等于放弃秒开） |
| F-4 | `open()` 失败路径删掉重复的 `closeMedia()` |

验证：服务端 `cm_logic_tests` 168/168、`test_business_messages` 23/23（真实 MySQL/Redis）；两者在 ASan+UBSan+LSan 下同样全绿、0 报告；客户端 `ctest` 10/10。

### 收尾轮完成情况（本轮）

| 条目 | 处理 |
|---|---|
| E-1 覆盖 | 新增 `TransferQueueDownloadDone.TruncatesStalePartTail`：造 8192 字节旧 `.part` + 4096 字节新文件，走 open/range/done 全链路，断言状态 Done、正式文件恰为 4096、`.part` 已重命名。**已验证用例有效性**：临时禁用截断后该用例失败 |
| C-4 | `decrementBlobRef` 在 `affectedRows()==0` 时记 WARN（指出可能重复减计数），仍返回 Ok——守卫已保证不会减成负数，改成返回错误会让用户删除直接失败，得不偿失 |
| C-6 | `purgeOne` 补 unlink `<blob>.meta`；`GcDeletesBlobAndSidecar` 增加 `.meta` 断言并把 unlink 调用数 2→3 |
| F-3 | `availableBytes()` 改为返回 `readableBytes()`，不再返回会超前的 `QFile::size()` |

### ASan / UBSan / LSan 结果（本轮）

`-DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug` 独立构建，17 个测试二进制、**305 个用例全部通过，0 条 sanitizer 报告**（`detect_leaks=1:halt_on_error=1`）。这是项目第一次有 ASan 基线。

### 本轮补完的四项（F-2 / D-2 / B-3 / C-3）

| 条目 | 改法 |
|---|---|
| F-2 | 音频 `avcodec_send_packet` 遇 `EAGAIN` 先 drain 再重发同一包，不再丢包 |
| D-2 | 新增 `cm/base/hex.h::toLowerHex`；收片时归一 `chunkMd5`，`doInit` 归一 `fileMd5`（都影响 blob 路径与逐片比对） |
| B-3 | 业务池投递失败改为 `queueInLoop` 回调，不在 `onData` 栈内重入 `fail/forceClose` |
| C-3 | 新增 `FileDao::markBlobMissing`（`f_state 1 → 3`）；`download open` 发现物理文件不存在时标记但**不改变本次请求结果**（读块时仍由 block_sender 回 3001）；`materializeInstantUpload` 增加"非就绪态一律拒绝"兜底 |

C-3 的取舍：open 阶段若直接失败会破坏现有测试构造（只建元数据不建文件），也会让"文件暂时不在但稍后恢复"的场景提前失败。因此选择**只标记不改结果**——秒传立刻停止命中，真实上传即可自愈。

### 审查方法上的经验

- 提交频次 + 危险写法分布能有效圈定高危文件，本次命中的 4 个 P1 有 3 个落在这些文件里。
- **必须读完一条链路再下结论**。E-2 差一点被误报成"并发额度泄漏"，实际 `isActive()` 不含 `Queued`，逻辑是对的。
- 先搜短名 `verifyCrc` 会漏掉 `verifyFrameCrc`，差点误报"分片 CRC 校验整体缺失"。关键词要先宽后窄。
- 测试缺口：服务端 42 个用例在**连真实 MySQL 后全绿**（`CM_TEST_MYSQL=1`，本次已验证），但**没有一条用例覆盖本次修的竞态/并发缺陷**（A-1 悬垂引用、C-1 秒传与 GC 竞态、E-1 .part 不截断、F-1 EAGAIN 丢包）。这些修复目前只有代码推理，没有回归防线——这是项目下一步最该补的东西。
- 验证进展：C-1 已补集成用例并通过（见上）。E-1 仍未覆盖，可用临时目录构造"旧 .part 更大"的场景，不需要网络。

