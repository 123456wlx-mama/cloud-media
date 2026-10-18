# Phase 2 实施计划：公共层（logger → codec → config → errorcode）

**Created:** 2026-10-18
**Status:** Not Started
**Source Spec:** docs/specs/02-协议设计.md；docs/specs/03-日志设计.md；终端改造路线图 Phase 2

## Overview

**Goal:** 在 common 层交付可复用的异步日志、协议 v2 Codec、INI 配置与错误码，并用 GoogleTest 建立跨 Windows MinGW / Linux 的回归基线。

**Approach:** 先搭 GTest + CTest 基础设施，并落下 ErrorCode 枚举骨架（避免 logger/config 二次改签名）；随后按 logger → codec → config → errorcode 完整化推进；最后跑 1M 行日志基准、双平台构建并提交。

**Tech Stack:** 纯 C++11、CMake 3.16+、GoogleTest 1.12.1（FetchContent）、CTest；无 Qt、无 FFmpeg、无数据库依赖。

## Current State

- Phase 1 骨架已提交：cm_common、cm_protocol、两个占位可执行。
- common 目前只有 version.cpp 与 protocol/version.cpp；无 logger/codec/config/errorcode 实现。
- BUILD_TESTS 默认 ON，但没有任何 test target，ctest 为 0 用例。
- docs/specs/02-协议设计.md 与 03-日志设计.md 已完成，是本次实现的契约。
- Windows 客户端占位已编译通过；Linux 服务端需用 Docker/CI 验证。

## Files and Responsibilities

| 文件/模块 | 动作 | 职责 | 备注 |
|---|---|---|---|
| cmake/tests.cmake | Create | GTest FetchContent + GoogleTest 集成 | 固定 release-1.12.1 |
| cmake/options.cmake | Modify | 新增 BUILD_BENCHMARKS（默认 ON） | 不影响现有选项 |
| CMakeLists.txt | Modify | BUILD_TESTS 时先 include(tests)，再 add_subdirectory(common)；BUILD_BENCHMARKS 时加 benchmarks | GTest target 必须先存在 |
| common/CMakeLists.txt | Modify | 增加 base/log/config/protocol 源文件；BUILD_TESTS 时加 tests 子目录 | 保持 target 名 |
| common/base/error_code.h/.cpp | Create | ErrorCode/ProtocolError 枚举；Task 5 补 to_string/is_retryable | Task 1 先落枚举骨架 |
| common/base/buffer.h/.cpp | Create | 可增长字节缓冲、读写索引、compact | Codec 的半包/粘包基础 |
| common/base/byte_order.h | Create | u16/u32/u64 大端转换 | 不依赖 htons |
| common/base/crc32.h/.cpp | Create | 流式与一次性 CRC32 | 多项式 0xEDB88320 |
| common/base/time_util.h/.cpp | Create | 本地时间毫秒格式化 | Windows localtime_s / POSIX localtime_r |
| common/log/log_level.h | Create | LogLevel、levelName、parseLogLevel | TRACE~FATAL/OFF |
| common/log/log_config.h | Create | LogConfig 字段与默认值 | 与 03 第 3 节一致 |
| common/log/logger.h/.cpp | Create | 单例、双缓冲、后台线程、滚动、traceId、flush | 唯一写文件者：后台线程 |
| common/config/ini_config.h/.cpp | Create | INI 解析、类型读取、错误定位 | 支持 BOM/注释/引号 |
| common/config/server_config.h/.cpp | Create | ServerConfig + 校验 + describe 脱敏 | 服务端 IP/端口/线程/DB/Redis |
| common/protocol/frame.h | Create | 魔数/版本/帧头常量/DecodeStatus/Frame | 复用 protocol/version.h |
| common/protocol/message_type.h | Create | 10000~10029 + 11000~11005 类型表 | 与 02 第 3 节一致 |
| common/protocol/serialize.h/.cpp | Create | Writer/Reader：u8/u16/u32/u64/string/bytes | 大端；长度前缀 |
| common/protocol/codec.h/.cpp | Create | 帧 encode/decode、CRC 校验、半包粘包 | 错误不投递业务 |
| common/tests/test_util.h | Create | 临时目录、JoinPath、RemoveTree、CountLines | 测试专用，跨平台 |
| common/tests/test_version.cpp | Create | 版本冒烟 | Task 1 |
| common/tests/test_logger.cpp | Create | 日志 7 例 | Task 2 |
| common/tests/test_ini_config.cpp | Create | INI 6 例 | Task 4 |
| common/tests/test_server_config.cpp | Create | ServerConfig 校验/脱敏 | Task 4 |
| common/tests/test_error_code.cpp | Create | 错误码 4 例 | Task 5 |
| common/protocol/tests/test_buffer.cpp | Create | Buffer 4 例 | Task 3 |
| common/protocol/tests/test_crc32.cpp | Create | CRC 3 例 | Task 3 |
| common/protocol/tests/test_serialize.cpp | Create | 序列化 3 例 | Task 3 |
| common/protocol/tests/test_codec.cpp | Create | Codec 8 例 | Task 3 |
| common/tests/CMakeLists.txt | Create | cm_common_tests + gtest_discover_tests | 源文件随任务增加 |
| common/protocol/tests/CMakeLists.txt | Create | cm_protocol_tests | Task 3 |
| benchmarks/CMakeLists.txt | Create | cm_log_bench | Task 2 |
| benchmarks/log_bench.cpp | Create | 1M 行 sync/async 基准 | Task 2/6 |
| docs/benchmark/日志性能.md | Create | 实测数据表 | Task 6 |

## Assumptions and Dependencies

- GTest 使用 FetchContent 拉取 release-1.12.1；1.13+ 需要 C++14，禁止升级。网络不可用或 CMake 4.x 对旧 policy 报错时，按风险表降级。
- 测试与基准只在 BUILD_TESTS / BUILD_BENCHMARKS 开启时构建，不进入服务端/客户端可执行文件。
- 本阶段不连接 MySQL/Redis；Config 只做解析与校验。
- 日志测试使用独立临时目录；Logger 是单例，每个用例 SetUp/TearDown 必须 shutdown，避免状态串扰。
- Windows 使用 MinGW Makefiles；Linux 使用默认 Makefiles/Ninja；两端都必须能编 common/protocol 与测试。
- 所有新增代码纯 C++11，不引入 Qt、FFmpeg、std::filesystem。
- 任务顺序按 logger → codec → config → errorcode；ErrorCode/ProtocolError 的枚举骨架在 Task 1 先落，完整描述表与测试仍在 Task 5，避免 logger/config 二次修改返回类型。

## Test Inventory（总数 37，≥15）

| Task | 测试文件 | 用例数 | 用例名 |
|---|---|---|---|
| 1 | common/tests/test_version.cpp | 2 | CommonVersionIsNotEmpty、ProtocolVersionIsTwo |
| 2 | common/tests/test_logger.cpp | 7 | LevelFiltering、FormatContainsFields、TraceIdIsEmitted、ConcurrentWritesNotLost、SizeRollingKeepsAllLines、ShutdownFlushes、SyncAsyncSameCount |
| 3 | common/protocol/tests/test_buffer.cpp | 4 | AppendAndRead、PartialRetrieve、GrowsForLargePayload、CompactReusesSpace |
| 3 | common/protocol/tests/test_crc32.cpp | 3 | KnownVector、EmptyInput、StreamingEqualsOneShot |
| 3 | common/protocol/tests/test_serialize.cpp | 3 | PrimitiveRoundTrip、StringRoundTrip、TruncatedStringFails |
| 3 | common/protocol/tests/test_codec.cpp | 8 | EncodeDecodeNormal、HalfPacketAtHeaderBoundaries、StickyPackets、BadMagic、BadVersion、BadLength、BadCrc、UnknownType |
| 4 | common/tests/test_ini_config.cpp | 4 | ParsesSectionsCommentsQuotesBom、MissingOptionalUsesDefault、InvalidIntegerReportsError、DuplicateKeyLastWins |
| 4 | common/tests/test_server_config.cpp | 2 | MissingRequiredFailsWithKey、RangeValidationAndPasswordRedaction |
| 5 | common/tests/test_error_code.cpp | 4 | KnownCodesToString、UnknownCodeFallsBack、RetryableClassification、ProtocolCodesAreStable |

合计：2 + 7 + 4 + 3 + 3 + 8 + 4 + 2 + 4 = 37 例。

## Task 1：GTest/CTest 基础设施 + ErrorCode 枚举骨架

**Goal:** 让 BUILD_TESTS 在 Windows MinGW 与 Linux 都能自动获得 GTest 并跑 CTest；同时落下 ErrorCode/ProtocolError 枚举，供后续任务使用。

**Files:**
- Create: cmake/tests.cmake
- Create: common/tests/CMakeLists.txt
- Create: common/tests/test_version.cpp
- Create: common/tests/test_util.h
- Create: common/base/error_code.h
- Modify: CMakeLists.txt
- Modify: common/CMakeLists.txt

**Steps:**

- [ ] 创建 cmake/tests.cmake：

~~~cmake
include(FetchContent)

FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG release-1.12.1)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)
~~~

- [ ] 修改根 CMakeLists.txt：在 add_subdirectory(common) 之前加入 BUILD_TESTS 分支（enable_testing + include(tests)）。
- [ ] 在 common/base/error_code.h 定义两个 enum class 骨架：
  - ErrorCode：Ok=0、1001~1009 通用、1099 UnknownError、2001~2004 认证/分享、3001~3006 传输。
  - ProtocolError：9001~9008（BadFrame/Version/UnknownType/PayloadTooLarge/Crc/HandshakeRequired/Deprecated/Timeout）。
  - 只定义枚举；to_string/is_retryable 在 Task 5 实现。
- [ ] 创建 common/tests/test_util.h：提供 TempDir、JoinPath、EnsureDir、RemoveTree、CountLines；Windows 用 _mkdir/_rmdir/GetCurrentProcessId，POSIX 用 mkdir/rmdir/getpid；不引入 C++17。
- [ ] 创建 common/tests/test_version.cpp，2 例：
  - CommonVersionIsNotEmpty：cm::version() 非空。
  - ProtocolVersionIsTwo：cm::protocol::version() == 2。
- [ ] 创建 common/tests/CMakeLists.txt：

~~~cmake
add_executable(cm_common_tests
    test_version.cpp
)
target_link_libraries(cm_common_tests PRIVATE cm_protocol GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(cm_common_tests)
~~~

- [ ] 在 common/CMakeLists.txt 末尾加入 BUILD_TESTS 时的 add_subdirectory(tests)。
- [ ] 构建与测试：

~~~bash
cmake -S . -B build -G "MinGW Makefiles" -DBUILD_SERVER=OFF -DBUILD_CLIENT=ON -DBUILD_TESTS=ON
cmake --build build -j8
ctest --test-dir build --output-on-failure
~~~

  - Expected：GTest 被下载/编译；ctest 2/2 通过，输出 VersionTest 两例。

**Security/Boundary Notes:** GTest 只用于 BUILD_TESTS，不链接进 cloud-media-server/client；FetchContent 不写入源码目录之外的可执行产物。

**Risks:** 网络不可用、CMake 4.x policy、MinGW g++ 16 与 GTest 1.12.1 兼容性；处理见第 11 节。

---

## Task 2：Logger（双缓冲异步 + 滚动 + traceId + 基准）

**Goal:** 实现 03-日志设计的核心能力，并用 7 个单测与 1M 行基准验收。

**Files:**
- Create: common/log/log_level.h
- Create: common/log/log_config.h
- Create: common/log/logger.h/.cpp
- Create: common/base/time_util.h/.cpp
- Create: common/tests/test_logger.cpp
- Create: benchmarks/CMakeLists.txt
- Create: benchmarks/log_bench.cpp
- Modify: cmake/options.cmake（新增 BUILD_BENCHMARKS，默认 ON）
- Modify: CMakeLists.txt（BUILD_BENCHMARKS 时 add_subdirectory(benchmarks)）
- Modify: common/CMakeLists.txt
- Modify: common/tests/CMakeLists.txt（加入 test_logger.cpp）

**Interface:**

~~~cpp
enum class LogLevel { Trace, Debug, Info, Warn, Error, Fatal, Off };

struct LogConfig {
    std::string path;
    LogLevel level;
    std::size_t max_size_bytes;
    int roll_days;
    bool async;
    bool console;
    int flush_interval_ms;
    std::size_t max_pending_bytes;
    bool fsync_on_fatal;
    std::size_t max_line_bytes;
};

class Logger {
public:
    static Logger& instance();
    ErrorCode init(const LogConfig& cfg, std::string* err);
    void shutdown();
    void setLevel(LogLevel level);
    bool enabled(LogLevel level) const;
    void log(LogLevel level, const char* file, int line, const std::string& msg);
    void flush();
};
~~~

**Steps:**

- [ ] 先写 common/tests/test_logger.cpp 的 7 个用例，再实现 Logger：
  1. LevelFiltering：level=Warn，写 Info 与 Error，文件只含 Error。
  2. FormatContainsFields：行含时间、级别、tid=、test_logger.cpp:、trace=、消息。
  3. TraceIdIsEmitted：setTraceId("abc123") 后写入，行含 trace=abc123。
  4. ConcurrentWritesNotLost：8 线程 × 1000 行，shutdown 后正好 8000 行。
  5. SizeRollingKeepsAllLines：max_size 4KB，写 200 行，文件数 >1，总行数 200。
  6. ShutdownFlushes：写 10 行后 shutdown，文件非空。
  7. SyncAsyncSameCount：async=false 与 true 各写 100 行，两种模式各 100 行。
- [ ] 实现 time_util：本地时间 + 毫秒，Windows localtime_s，POSIX localtime_r；不依赖 Qt。
- [ ] 实现 log_level：levelName、parseLogLevel（大小写不敏感）。
- [ ] 实现 Logger：
  - front/back 各为 vector of string；mutex 只保护 push/swap；后台线程写文件。
  - 每行在业务线程格式化；tid 为进程内自增逻辑线程号；traceId 为 thread_local。
  - 批量阈值 256 行或 64KB；flush_interval_ms 定时 flush。
  - 滚动：大小 + 日期；文件名 server.log.YYYYMMDD-HHMMSS.N；保留 roll_days。
  - 背压：pending 超过 max_pending 先丢 TRACE/DEBUG，再丢 WARN；计数 dropped。
  - shutdown 幂等；析构兜底 shutdown；FATAL 立即 flush。
  - 日志宏 CM_LOG_TRACE/DEBUG/INFO/WARN/ERROR/FATAL，先判断 enabled 再构造消息。
- [ ] 实现 benchmarks/log_bench.cpp：--count、--threads、--sync/--async、--file；输出 mode/threads/count/elapsed_ms/lines_per_sec/file_size。
- [ ] benchmarks/CMakeLists.txt：add_executable(cm_log_bench log_bench.cpp)，链接 cm_common，输出 build/bin。
- [ ] 修改 cmake/options.cmake 新增 BUILD_BENCHMARKS（默认 ON）；修改根 CMakeLists 在 BUILD_BENCHMARKS 时 add_subdirectory(benchmarks)。
- [ ] 运行：

~~~bash
cmake -S . -B build -G "MinGW Makefiles" -DBUILD_SERVER=OFF -DBUILD_CLIENT=ON -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON
cmake --build build -j8
ctest --test-dir build -R "LoggerTest" --output-on-failure
./build/bin/cm_log_bench --async --count 1000000 --threads 1 --file build/log_async.log
./build/bin/cm_log_bench --sync  --count 1000000 --threads 1 --file build/log_sync.log
~~~

  - Expected：7/7 通过；两条基准输出 elapsed_ms 与 lines_per_sec。

**Security/Boundary Notes:** 日志禁止输出密码/token/payload；POSIX 文件 0600；Windows 依赖 ACL；shutdown 后丢弃并写 stderr。

**Risks:** 单例跨用例污染、Windows 文件锁导致 RemoveTree 失败、滚动测试受时间影响；处理见第 11 节。

---

## Task 3：Codec（Buffer + CRC32 + 序列化 + 帧编解码）

**Goal:** 按 02-协议设计实现 v2 帧编解码与粘包/半包处理，并用 18 个单测覆盖边界。

**Files:**
- Create: common/base/buffer.h/.cpp
- Create: common/base/byte_order.h
- Create: common/base/crc32.h/.cpp
- Create: common/protocol/frame.h
- Create: common/protocol/message_type.h
- Create: common/protocol/serialize.h/.cpp
- Create: common/protocol/codec.h/.cpp
- Create: common/protocol/tests/test_buffer.cpp
- Create: common/protocol/tests/test_crc32.cpp
- Create: common/protocol/tests/test_serialize.cpp
- Create: common/protocol/tests/test_codec.cpp
- Create: common/protocol/tests/CMakeLists.txt
- Modify: common/CMakeLists.txt
- Modify: common/CMakeLists.txt 中 BUILD_TESTS 时加入 protocol/tests

**Steps:**

- [ ] 先写 Buffer 4 例：AppendAndRead、PartialRetrieve、GrowsForLargePayload、CompactReusesSpace。
- [ ] 实现 Buffer：vector of char + readerIndex/writerIndex；append/peek/readableBytes/retrieve/retrieveAsString/ensureWritable/compact；扩容上限 16MB。
- [ ] 先写 CRC 3 例：KnownVector（"123456789" → 0xCBF43926）、EmptyInput（0）、StreamingEqualsOneShot。
- [ ] 实现 Crc32 类（update/value）与一次性 crc32；表用函数内 static 初始化。
- [ ] 先写 Serialize 3 例：PrimitiveRoundTrip（u8/u16/u32/u64）、StringRoundTrip（含中文）、TruncatedStringFails。
- [ ] 实现 Writer/Reader：大端；string = u32 长度 + UTF-8；bytes = u32 长度；Reader 失败后进入错误态。
- [ ] 定义 frame.h：kMagic=0x434D4456、kVersion=2、kHeaderSize=19、kMaxPayload=8MB；DecodeStatus；Frame。
- [ ] 定义 message_type.h：10000~10029 全部旧语义 + 11000~11005 控制类型；isKnownType。
- [ ] 先写 Codec 8 例：EncodeDecodeNormal、HalfPacketAtHeaderBoundaries、StickyPackets、BadMagic、BadVersion、BadLength、BadCrc、UnknownType。
- [ ] 实现 Codec::encode/decode：
  - 帧头顺序：MAGIC(4) VERSION(1) TYPE(2) LEN(4) SEQ(4) CRC32(4)。
  - CRC 覆盖偏移 4 到 19+LEN-1；LEN 仅 payload。
  - 半包 NeedMore 不消费；Ok 才 retrieve 整帧；错误不消费并交由调用方断开。
  - 未知类型返回 UnknownType，不进入业务分发。
- [ ] 创建 protocol/tests/CMakeLists.txt，target cm_protocol_tests，链接 cm_protocol 与 GTest::gtest_main。
- [ ] 运行：

~~~bash
cmake --build build -j8
ctest --test-dir build -R "BufferTest|Crc32Test|SerializeTest|CodecTest" --output-on-failure
~~~

  - Expected：18/18 通过。

**Security/Boundary Notes:** LEN 先校验再分配；Buffer 上限 16MB；CRC 只做完整性；错误日志不打印 payload。

**Risks:** 帧头偏移/CRC 范围 off-by-one；三处长度（LEN、可读字节、payload 长度）不一致；处理见第 11 节。

---

## Task 4：Config（INI 解析 + ServerConfig 校验）

**Goal:** 实现 03 第 12 节的 INI 配置与 ServerConfig 校验，服务端启动前 fail fast。

**Files:**
- Create: common/config/ini_config.h/.cpp
- Create: common/config/server_config.h/.cpp
- Create: common/tests/test_ini_config.cpp
- Create: common/tests/test_server_config.cpp
- Modify: common/CMakeLists.txt
- Modify: common/tests/CMakeLists.txt

**Interface:**

~~~cpp
class IniConfig {
public:
    ErrorCode load(const std::string& path, std::string* err);
    bool has(const std::string& section, const std::string& key) const;
    std::string getString(const std::string& section, const std::string& key,
                          const std::string& def) const;
    int getInt(const std::string& section, const std::string& key, int def,
               bool* ok) const;
    bool getBool(const std::string& section, const std::string& key, bool def,
                 bool* ok) const;
};

ErrorCode loadServerConfig(const std::string& path, ServerConfig* out,
                           std::string* err);
~~~

**Steps:**

- [ ] 先写 INI 4 例：ParsesSectionsCommentsQuotesBom、MissingOptionalUsesDefault、InvalidIntegerReportsError、DuplicateKeyLastWins。
- [ ] 实现 INI：BOM 跳过；#/; 注释；[section]；key=value；trim；单双引号；重复 key 后者覆盖；错误含行号。
- [ ] 定义 ServerConfig：listen_ip、port、io_threads、worker_threads、data_dir、log 参数、mysql 参数、redis 参数、transfer 参数；字段与 03 表一致。
- [ ] 先写 ServerConfig 2 例：MissingRequiredFailsWithKey（缺 mysql.user，err 含 key）、RangeValidationAndPasswordRedaction（port 70000 失败；describe 不含 password）。
- [ ] 实现 loadServerConfig：读取 [server]/[log]/[mysql]/[redis]/[transfer]；校验端口 1~65535、线程 1~64、必填项非空、log level 可解析；失败返回对应 ErrorCode。
- [ ] 运行：

~~~bash
cmake --build build -j8
ctest --test-dir build -R "IniConfigTest|ServerConfigTest" --output-on-failure
~~~

  - Expected：6/6 通过。

**Security/Boundary Notes:** 密码不进入 describe/日志；错误信息只写 key 名；配置文件权限由部署脚本控制。

**Risks:** BOM/引号/行尾注释边界；Windows 路径分隔符；密码回显；处理见第 11 节。

---

## Task 5：ErrorCode 完整化（to_string/is_retryable + ProtocolError）

**Goal:** 补齐错误码描述表、可重试判断与协议错误码名称，并让 logger/config 的返回码统一。

**Files:**
- Create: common/base/error_code.cpp
- Modify: common/base/error_code.h
- Create: common/tests/test_error_code.cpp
- Modify: common/tests/CMakeLists.txt
- Modify: common/log/logger.cpp（如仍需调整返回码）
- Modify: common/config/*.cpp（如仍需调整返回码）

**Steps:**

- [ ] 先写 4 例：KnownCodesToString、UnknownCodeFallsBack、RetryableClassification、ProtocolCodesAreStable。
- [ ] 实现 to_string(ErrorCode)：覆盖 0、1001~1009、1099、2001~2004、3001~3006；未知值返回 "UnknownError"。
- [ ] 实现 is_retryable：Timeout/NetworkError/ChunkMissing/Md5Mismatch/RateLimited 返回 true；AuthFailed/InvalidArgument 等 false。
- [ ] 实现 to_string(ProtocolError)：9001~9008 对应 ERR_BAD_FRAME/ERR_VERSION/ERR_UNKNOWN_TYPE/ERR_PAYLOAD_TOO_LARGE/ERR_CRC/ERR_HANDSHAKE_REQUIRED/ERR_DEPRECATED/ERR_TIMEOUT。
- [ ] 检查 logger/config：init/load 的失败返回 ErrorCode，不再混用 bool；错误详情用 err 字符串补充字段名。
- [ ] 运行：

~~~bash
cmake --build build -j8
ctest --test-dir build -R "ErrorCodeTest" --output-on-failure
~~~

  - Expected：4/4 通过。

**Security/Boundary Notes:** 数值一旦发布不得修改；未知值必须兜底；错误信息不回显密码。

**Risks:** 枚举值与 02/03 文档不一致；处理见第 11 节。

---

## Task 6：全量验证、日志基准与提交

**Goal:** 双平台构建、跑满 37 例、产出 1M 行日志数据，按路线图提交 Phase 2。

**Files:**
- Create: docs/benchmark/日志性能.md
- Modify: README.md（当前状态补 Phase 2 与测试命令）
- Modify: docs/specs/02-协议设计.md / 03-日志设计.md（仅当实现与设计有偏差时同步）

**Steps:**

- [ ] Windows 全量构建与测试：

~~~bash
cmake -S . -B build -G "MinGW Makefiles" -DBUILD_SERVER=OFF -DBUILD_CLIENT=ON -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON
cmake --build build -j8
ctest --test-dir build --output-on-failure
~~~

  - Expected：ctest 37/37；无编译警告；cloud-media-client --version 仍正常。

- [ ] Linux 构建与测试（Docker）：

~~~bash
docker run --rm -v D:/cloud-media:/src -w /src ubuntu:22.04 bash -lc "apt-get update -qq && apt-get install -y -qq cmake g++ git && cmake -S . -B build-linux -DBUILD_SERVER=ON -DBUILD_CLIENT=OFF -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON && cmake --build build-linux -j4 && ctest --test-dir build-linux --output-on-failure"
~~~

  - Expected：37/37 通过；cloud-media-server 可构建。

- [ ] 1M 行日志基准：

~~~bash
./build/bin/cm_log_bench --sync  --count 1000000 --threads 1 --file build/log_sync.log
./build/bin/cm_log_bench --async --count 1000000 --threads 1 --file build/log_async.log
~~~

  - 把 mode/threads/count/elapsed_ms/lines_per_sec/file_size 写入 docs/benchmark/日志性能.md，注明机器、编译器、构建类型。

- [ ] 红线检查：

~~~bash
grep -RIn "QObject\|QString\|#include <Q" common/ || echo clean-qt
grep -RIn "libav\|SDL_" common/ || echo clean-media
grep -RIn "std::make_unique\|std::optional\|string_view\|std::filesystem" common/ || echo clean-cpp14
~~~

  - Expected：三条均输出 clean-*。

- [ ] 提交（路线图要求每阶段一次提交）：

~~~bash
git add -A
git commit -m "feat(common): 实现异步日志、协议 v2、配置与错误码"
~~~

## Verification

- [ ] Windows MinGW：cmake configure/build 通过，ctest 37/37。
- [ ] Linux Docker：服务端构建通过，ctest 37/37。
- [ ] GTest 1.12.1 由 FetchContent 获取，未引入 C++14。
- [ ] logger：8 线程 × 1000 行无丢失；滚动不丢行；shutdown flush。
- [ ] codec：半包/粘包/坏魔数/坏版本/超长/坏 CRC/未知类型全部覆盖。
- [ ] config：BOM/注释/引号/重复 key/非法整数/缺必填/端口范围/密码脱敏。
- [ ] errorcode：已知/未知/可重试/协议码稳定。
- [ ] 1M 行 sync/async 数据写入 docs/benchmark/日志性能.md。
- [ ] 红线检查三条 clean。
- [ ] 工作区干净，Phase 2 提交存在。

## Risks and Mitigations

| 风险 | 影响 | 对策 |
|---|---|---|
| GTest FetchContent 网络不可用 | configure 失败 | 预置 thirdparty/googletest 或系统包；文档写明离线方案 |
| CMake 4.x 对 GTest 1.12.1 的旧 policy 报错 | configure 失败 | 在 tests.cmake 中按 CMake 版本设置 CMAKE_POLICY_VERSION_MINIMUM=3.5；仍失败则用 tarball |
| GTest 1.13+ 需要 C++14 | 违反纯 C++11 | 固定 release-1.12.1，禁止升级 |
| MinGW g++ 16 编译 GTest 1.12.1 警告/错误 | Windows 测试失败 | 不设 -Werror；必要时给 gtest target 单独降级警告 |
| Logger 单例跨用例污染 | 测试互相影响 | 每个用例 SetUp/TearDown shutdown；唯一临时目录 |
| Windows 文件锁导致清理失败 | 测试残留/失败 | 先 shutdown 再 RemoveTree；失败不阻塞用例结论 |
| 滚动测试受时间/文件系统影响 | 偶发失败 | 只测大小滚动；日期滚动用注入 Clock |
| 帧头偏移或 CRC 范围 off-by-one | 解码全错 | 用固定字节向量与边界用例锁定偏移；代码评审重点 |
| 三处长度不一致（LEN/可读/payload） | 越界/半包误判 | encode 后断言；decode 只在 19+LEN 可读时消费 |
| Config 行尾注释/引号/BOM 边界 | 解析错误 | 用例覆盖；解析规则写入 03 文档 |
| 日志/配置密码泄露 | 安全 | describe/err 不含密码；红线 grep；评审检查 |
| Linux 本机不可验证 | DoD 缺口 | Docker/CI 跑 server + tests；记录命令与结果 |
| 基准数字受机器影响 | 误读 | 文档注明环境；只记录实测，不承诺固定倍数 |

## Rollback

- 本阶段全部为 common 新文件与 CMake 增量修改，不涉及数据库/线上数据；回滚使用 git revert 对应提交。
- GTest 只影响 BUILD_TESTS；关闭 BUILD_TESTS 即可恢复 Phase 1 构建。
- 日志/协议未接入服务端主流程前，回滚不影响占位可执行。

## Deferred Decisions

- JSON 配置：Phase 2 只做 INI；后续新增 JsonConfig 适配层，不改 IniConfig 接口。
- 日志热调整级别：Phase 2 不做；后续通过管理接口/信号实现。
- 日志压缩归档：不做；按 roll_days 删除。
- 跨进程 traceId：不做；Phase 3 只做进程内 thread_local + 任务携带。
- Fuzz 1M 随机输入：Phase 6 做；Phase 2 只做边界单测。
- 客户端 ClientConfig：Phase 8 接入 Qt 时再做；本阶段只做 ServerConfig。
