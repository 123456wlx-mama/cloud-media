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
