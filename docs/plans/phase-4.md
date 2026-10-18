# Phase 4 实施计划（存储层：MySQL / Redis / 缓存）

**Created:** 2026-10-18
**Status:** In Progress（Stage 1-4 代码已落盘；未执行构建/测试，待人工验收）
**Implementation Note:** RedisPool 与 CacheStore 按用户要求落在 server/cache/，不采用原计划的 server/pool/redis_*；其余结构不变。
**Source Spec:** docs/specs/05-存储设计.md（v0.2，5 条关键取舍已评审）
**Review:** 2026-10-18 用户确认两项实施取舍：拆分建库脚本（测试库物理隔离）、缓存用 setPx 毫秒 TTL
**Execution Policy:** 本文件中的构建、测试、建库命令均只作为验收文本，由人工执行；本会话不运行任何命令。

## Overview

**Goal:** 在现有 Phase 1~3 骨架上落地存储层：schema v2、MysqlPool、RedisPool、Cache-Aside 封装与两档测试（无服务单测 + 真服务集成测试），为 Phase 5 业务实现提供可直接调用的基础设施。

**Approach:** 严格按「schema → MySQL → Redis → 缓存 → 集成验收」顺序推进，每阶段结束时都必须能构建、能跑该阶段的测试。所有需要真实服务的测试默认不注册为必跑用例；未设置环境变量时用 GTEST_SKIP 跳过，设置后连接失败则视为失败。

**Tech Stack:** C++11、CMake 3.5、GoogleTest 1.12.1（thirdparty/googletest）、libmysqlclient（MySQL 5.7）、hiredis 0.13、Redis 3.2+、Ubuntu 16.04 / g++ 5.4。

## Current Baseline

已完成并可复用：

- Phase 1：单仓库骨架、cm_common / cm_protocol、build/bin 输出约定。
- Phase 2：日志、配置、协议 v2、CRC32、Buffer、ErrorCode。
- Phase 3：EventLoop / Channel / EpollPoller / TcpConnection / TimerQueue / ThreadPool；server/net 的测试已用 add_test 显式注册。
- 测试基础设施：BUILD_TESTS 默认 ON，cmake/tests.cmake 引入 thirdparty/googletest，GTest 1.12.1 支持 GTEST_SKIP。
- 现有约定：CMake 3.5、C++11、可执行文件统一输出 build/bin、禁止 FetchContent/联网。

本阶段开始前不存在：

- BUILD_STORAGE 选项、cm_storage target、libmysqlclient/hiredis 的查找逻辑。
- sql/ 下的建表脚本（只有 .gitkeep）。
- server/pool、server/db、server/cache 下的任何实现（只有 .gitkeep）。
- server/tests 下的测试与 CMakeLists。

## Files and Responsibilities

| 文件/模块 | 动作 | 职责 | 备注 |
|---|---|---|---|
| sql/create_database.sql | Create | 生产库建库语句（仅 cloud_media） | 测试库由验收命令单独创建，避免部署脚本建测试库 |
| sql/schema_v2.sql | Create | 7 张表的 DDL，不含 CREATE DATABASE/USE | 与 05 附录 A 等价，拆分原因见 Task 1.1 |
| scripts/reconcile.sql | Create | 5 条不变量的只读对账 SQL | 不自动修数据 |
| cmake/options.cmake | Modify | BUILD_STORAGE、ENABLE_DB_INTEGRATION_TESTS | Linux 默认 ON，Windows 默认 OFF |
| cmake/thirdparty.cmake | Modify | cm_find_mysql / cm_find_hiredis | mysql_config 仅作 HINT |
| server/CMakeLists.txt | Modify | cm_storage target、服务端条件链接、add_subdirectory(tests) | 不新增目录层级 |
| common/include/cm/base/error_code.h | Modify | 新增 4xxx 存储错误码 | 数值在 Task 2.2 固定 |
| common/base/error_code.cpp | Modify | to_string / is_retryable 分支 | 与头文件同步 |
| common/tests/test_error_code.cpp | Modify | 存储错误码唯一性与可重试性断言 | 无外部依赖 |
| server/pool/mysql_pool.h/.cpp | Create | MysqlOptions、MysqlPoolStats、MysqlPool facade、describe | 不向业务暴露 MYSQL* |
| server/pool/mysql_conn.h/.cpp | Create | RAII 连接、ping、事务原语、prepare | 析构归还 |
| server/pool/prepared_statement.h/.cpp | Create | MYSQL_STMT 封装、两段式绑定、结果读取 | 不做语句缓存 |
| server/pool/redis_pool.h/.cpp | Create | RedisOptions、RedisPool facade、describe | 连接独占 |
| server/pool/redis_conn.h/.cpp | Create | 命令封装、Lookup、broken 标记 | redisCommandArgv |
| server/pool/redis_reply.h/.cpp | Create | redisReply 的 RAII 与类型化读取 | 防泄漏 |
| server/db/user_dao.h/.cpp | Create | 登录/注册所需最小 DAO | 池 API 的端到端样例 |
| server/db/file_dao.h/.cpp | Create | 按 MD5 查 blob 的最小 DAO | 秒传路径样例 |
| server/cache/cache_key.h/.cpp | Create | 统一 key 构造 | 与 05 §6.1 一致 |
| server/cache/cache_ttl.h/.cpp | Create | TTL 抖动（纯函数 + 随机包装） | 可单测边界 |
| server/cache/cache_store.h | Create | 缓存存储抽象（Fake 可替换） | 不依赖 Redis |
| server/cache/redis_cache_store.h/.cpp | Create | RedisConn 到 CacheStore 的适配 | 翻译 Lookup |
| server/cache/cache_aside.h/.cpp | Create | 读路径、空值缓存、单飞 | 锁失败回退 DB |
| server/cache/rebuild_guard.h/.cpp | Create | 本地 single-flight + Redis SET NX + Lua 释放 | 锁只削峰 |
| server/cache/rate_limiter.h/.cpp | Create | 固定窗口限流（Lua） | 与降级表配套 |
| server/tests/CMakeLists.txt | Create | cm_storage_tests 与两档 add_test | 无 gtest_discover_tests |
| server/tests/storage/test_env.h | Create | 环境变量读取与 GTEST_SKIP 约定 | 不打印密码 |
| server/tests/storage/test_*.cpp | Create | 单元与集成用例 | 见 gtest 清单 |

## Assumptions and Dependencies

- 目标机为 Ubuntu 16.04 / g++ 5.4 / cmake 3.5；MySQL 5.7 已开启 innodb_large_prefix，表使用 ROW_FORMAT=DYNAMIC。
- libmysqlclient-dev 与 libhiredis-dev 已由人工安装；本计划不含 apt/docker/联网步骤。
- MySQL 测试库为 cloud_media_test，账号需具备该库的 CREATE/INSERT/UPDATE/DELETE/SELECT 权限；生产库名 cloud_media 不在测试中使用。
- Redis 测试使用 db 15（可用 CM_TEST_REDIS_DB 覆盖），不执行 FLUSHALL/FLUSHDB；每个用例使用带进程号与前缀的独立 key。
- 所有凭据只从环境变量读取：CM_TEST_MYSQL_PASSWORD、CM_TEST_REDIS_PASSWORD；仓库与 CMake 中不出现密码。
- GTEST_SKIP 在 SetUp 中调用即跳过整个用例，不计失败；GTest 1.12.1 已满足。
- DAO 范围：本阶段只实现 UserDao 与 FileDao；TransferDao / ShareDao / PlayDao 留到 Phase 5（见 Deferred Decisions）。
- 不引入 statement 缓存、Redis pipeline、Sentinel、连接池第三方库。

## Task Map

| 阶段 | 交付物 | 通过门槛 |
|---|---|---|
| Stage 1 | schema + 对账 SQL | 7 表可建；唯一键/无外键/软删复用同名通过手工 SQL 验证 |
| Stage 2 | CMake + MysqlPool + 最小 DAO | 无 DB 单测全绿；有 DB 集成测试全绿；密码不出现在 describe |
| Stage 3 | RedisPool | 连接独占、超时即弃、TTL/INCR/Lua 用例通过 |
| Stage 4 | 缓存封装 | 穿透/击穿/雪崩三组用例通过；锁失败回退有计数 |
| Stage 5 | 集成验收 | storage_unit 无服务全绿；storage_integration 有服务全绿；ASan 无报告 |

---

## Stage 1：schema SQL 与对账脚本

### Task 1.1：拆分并落盘 schema_v2.sql

**Goal:** 把 05 附录 A 的 DDL 落成可重复执行的建表脚本，并拆出建库语句，使测试库能复用同一份表定义。

**Files:**
- Create: sql/create_database.sql
- Create: sql/schema_v2.sql

**Steps:**
- [ ] sql/create_database.sql 只包含一条语句：CREATE DATABASE IF NOT EXISTS cloud_media DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;
- [ ] sql/schema_v2.sql 顶部只写 SET NAMES utf8mb4;，不写 CREATE DATABASE / USE；其余 7 张表逐字对齐 05 附录 A（字段名、类型、默认值、COMMENT、索引名、ROW_FORMAT）。
- [ ] 表顺序固定为 t_user → t_file → t_user_file → t_chunk → t_transfer_task → t_share → t_play_record。
- [ ] 每张表必须显式 ROW_FORMAT=DYNAMIC、DEFAULT CHARSET=utf8mb4。
- [ ] 不建外键、不建触发器、不建视图。

**验收标准:**
- [ ] 脚本在空库上一次执行成功；重复执行会因表已存在而报错，这是预期（避免掩盖 schema 漂移），重跑前先 DROP DATABASE。
- [ ] 执行后 SHOW TABLES 返回恰好 7 张表。
- [ ] SHOW CREATE TABLE t_user_file 含 uk_user_parent_name、idx_list、idx_name、idx_recent、idx_file、ROW_FORMAT=DYNAMIC。
- [ ] 全文件搜索 FOREIGN KEY 结果为 0；搜索 REFERENCES 结果为 0。
- [ ] 全文件搜索 CREATE TRIGGER / CREATE VIEW 结果为 0。

---

### Task 1.2：对账脚本

**Goal:** 把 05 §2.11 的 5 条不变量固化成只读 SQL，作为每日巡检与迁移后校验的输入。

**Files:**
- Create: scripts/reconcile.sql

**Steps:**
- [ ] 写 5 段独立查询，每段前用注释标明 I1~I5 与「期望返回 0 行」。
- [ ] I1 引用计数：对比 f_ref_count 与实际未删除节点数。
- [ ] I2 悬空引用：LEFT JOIN t_file，筛 f_id 非空且父行缺失或 f_state<>1。
- [ ] I3 父链异常：自连接，筛跨用户或父节点非目录。
- [ ] I4 分片汇总：DONE 任务的 SUM(c_size) 与 tr_total_size 不一致。
- [ ] I5 分享失效：t_share JOIN t_user_file，筛节点已删或 s_expire_at < NOW(6)。
- [ ] 所有查询只读，不包含 UPDATE/DELETE/INSERT。

**验收标准:**
- [ ] 在 Task 1.3 造好的测试数据上执行，5 段均返回 0 行。
- [ ] 手工删除一条 t_user_file 后执行 I1，能返回 1 行差异；恢复后回到 0 行。
- [ ] 文件内 UPDATE/DELETE/INSERT 关键字计数为 0（注释除外）。

---

### Task 1.3：schema 手工验收

**Goal:** 用真实 MySQL 证明唯一键、软删复用、复合主键与字符集行为符合设计。

**Files:**
- Verify: sql/schema_v2.sql、scripts/reconcile.sql

**Steps:**
- [ ] 建测试库并导入表结构（人工执行）：

~~~bash
mysql -h 127.0.0.1 -u cm_test -p -e "source sql/create_database.sql"
mysql -h 127.0.0.1 -u cm_test -p cloud_media_test < sql/schema_v2.sql
~~~

- [ ] 检查行格式与索引前缀：

~~~sql
SELECT @@innodb_large_prefix, @@innodb_file_format;
SHOW CREATE TABLE t_user_file;
~~~

  - Expected: large_prefix=ON；t_user_file 的 DDL 含 ROW_FORMAT=DYNAMIC 与全部索引。

- [ ] 插入一个用户、一个 blob、根目录下同名节点两次：

~~~sql
INSERT INTO t_user (u_tel, u_password, u_salt, u_name) VALUES ('13800000001', REPEAT('a',64), REPEAT('b',32), 'tester');
INSERT INTO t_file (f_md5, f_size, f_path, f_ref_count, f_state) VALUES (REPEAT('c',32), 1024, 'cc/cc/x', 1, 1);
INSERT INTO t_user_file (u_id, uf_parent_id, f_id, uf_name) VALUES (1, 0, 1, 'a.txt');
INSERT INTO t_user_file (u_id, uf_parent_id, f_id, uf_name) VALUES (1, 0, 1, 'a.txt');
~~~

  - Expected: 第二条 INSERT 返回 ERROR 1062（uk_user_parent_name）。

- [ ] 软删后复用同名：

~~~sql
UPDATE t_user_file SET uf_deleted_at = NOW(6) WHERE uf_name='a.txt';
INSERT INTO t_user_file (u_id, uf_parent_id, f_id, uf_name) VALUES (1, 0, 1, 'a.txt');
~~~

  - Expected: INSERT 成功；表中两条 a.txt，一条哨兵值、一条真实删除时间。

- [ ] 验证目录节点 f_id 可为 NULL 且 f_id 非空节点必须能 JOIN 到 t_file（I2 返回 0 行）。
- [ ] 执行 scripts/reconcile.sql，5 段均返回 0 行。

**验收标准:**
- [ ] 上述 4 组 SQL 结果全部符合 Expected。
- [ ] utf8mb4 生效：插入含 emoji 的文件名（如 😀.mp4）成功，SELECT 原样返回。
- [ ] 测试数据在验收后删除，cloud_media_test 保持干净。

---

## Stage 2：CMake 接入 + MysqlPool + 最小 DAO

### Task 2.1：BUILD_STORAGE 与依赖定位

**Goal:** 让 Linux 能按选项构建 cm_storage，Windows 明确拒绝，且不引入联网依赖。

**Files:**
- Modify: cmake/options.cmake
- Modify: cmake/thirdparty.cmake
- Modify: CMakeLists.txt（状态输出增加 BUILD_STORAGE / ENABLE_DB_INTEGRATION_TESTS）
- Modify: server/CMakeLists.txt

**Steps:**
- [ ] options.cmake 追加 BUILD_STORAGE（WIN32 默认 OFF，其他默认 ON）、ENABLE_DB_INTEGRATION_TESTS（默认 OFF）。
- [ ] 根 CMakeLists.txt 的 message(STATUS) 增加 BUILD_STORAGE 与 ENABLE_DB_INTEGRATION_TESTS 两行。
- [ ] 增加三条保护：
  - BUILD_STORAGE 且 WIN32 → FATAL_ERROR，消息含「Linux-only」「libmysqlclient」「hiredis」。
  - BUILD_SERVER 且非 BUILD_STORAGE → WARNING，说明服务端将无持久化。
  - BUILD_STORAGE 且非 BUILD_SERVER → WARNING，说明 cm_storage 不会构建。
- [ ] thirdparty.cmake 增加 cm_find_mysql() 与 cm_find_hiredis()：
  - mysql_config 只用于提供 -I / -L 的 HINT；最终用 find_path(mysql.h) + find_library(mysqlclient/mariadb) 判定。
  - hiredis 用 find_path(hiredis/hiredis.h) + find_library(hiredis)。
  - 创建 cm::mysql、cm::hiredis 两个 UNKNOWN IMPORTED target。
  - 找不到时 FATAL_ERROR，消息给出 Ubuntu 包名，不自动下载。
- [ ] 本任务不创建 cm_storage（对应源码尚未存在），也不 add_subdirectory(tests)；target 在 Task 2.3 创建，测试子目录在 Task 2.7 加入。

**验收标准:**
- [ ] Linux 下 cmake 配置输出中包含 BUILD_STORAGE=ON；cmake --build 成功。
- [ ] 用 -DBUILD_STORAGE=OFF 配置仍成功，构建行为与 Phase 3 一致（此时尚未创建 cm_storage）。
- [ ] Windows 下 -DBUILD_STORAGE=ON 配置阶段直接失败，错误信息可读。
- [ ] 删除/改名 MYSQL_INCLUDE_DIR 后配置失败，提示安装 libmysqlclient-dev，不尝试下载。
- [ ] 全仓库无 FetchContent / ExternalProject / pkg_check_modules(... IMPORTED_TARGET)。

---

### Task 2.2：存储错误码

**Goal:** 让池层与缓存层有稳定的错误语义，且与既有 1xxx/2xxx/3xxx 段不冲突。

**Files:**
- Modify: common/include/cm/base/error_code.h
- Modify: common/base/error_code.cpp
- Modify: common/tests/test_error_code.cpp

**Steps:**
- [ ] 在 ErrorCode 中追加（固定数值，不再漂移）：
  - DbUnavailable = 4001
  - DbPoolTimeout = 4002
  - DbQueryFailed = 4003
  - DbTxConflict = 4004
  - RedisUnavailable = 4005
  - CacheRebuildFailed = 4006
  - AuthUnavailable = 4007
  - RateLimitUnavailable = 4008
- [ ] to_string 为每个新值返回固定英文短语（日志用），不返回中文，避免编码问题。
- [ ] is_retryable：DbUnavailable、DbPoolTimeout、DbTxConflict、RedisUnavailable、AuthUnavailable、RateLimitUnavailable 返回 true；DbQueryFailed、CacheRebuildFailed 返回 false。
- [ ] test_error_code.cpp 增加两个用例：新值唯一且落在 4001~4099；可重试性与上表一致。

**验收标准:**
- [ ] 编译无 warning。
- [ ] 新增用例通过；原有错误码数值不变（grep 断言 1001/2001/3001 仍存在）。
- [ ] to_string 对未知值返回 "UnknownError"，不崩溃。

---

### Task 2.3：MysqlOptions、describe 与错误分类

**Goal:** 把纯逻辑从连接池里剥离出来，使密码脱敏与错误映射可在无 DB 环境下单测。

**Files:**
- Create: server/pool/mysql_pool.h
- Create: server/pool/mysql_pool.cpp
- Modify: server/CMakeLists.txt

**Steps:**
- [ ] 定义 MysqlOptions（host/port/user/password/database/charset/minSize/maxSize/idleTimeout/acquireTimeout/maxLifetime/slowQuery），提供默认构造函数：127.0.0.1:3306、charset=utf8mb4、min=2、max=8、idle=60s、acquire=500ms、lifetime=30min、slow=200ms。
- [ ] 定义 MysqlPoolStats：total/idle/inUse/waiters/acquireTotal/acquireTimeoutTotal/createTotal/createFailTotal/slowQueryTotal/slowQueryMaxMs。
- [ ] 提供 std::string formatMysqlDescription(const MysqlOptions&, const MysqlPoolStats&)：输出 host/port/db/user 与池计数，**永不读取 password**。
- [ ] 提供 ErrorCode classifyMysqlError(unsigned int code)：
  - CR_SERVER_GONE_ERROR、CR_SERVER_LOST、CR_CONN_HOST_ERROR → DbUnavailable
  - 1213、1205 → DbTxConflict
  - 1062 → AlreadyExists
  - 其他 → DbQueryFailed
- [ ] 在 server/CMakeLists.txt 创建 cm_storage STATIC，先只包含 pool/mysql_pool.cpp；target_include_directories PUBLIC 当前目录；链接 PUBLIC cm_common、PRIVATE cm::mysql cm::hiredis Threads::Threads。
- [ ] cloud-media-server 在 BUILD_STORAGE 时追加链接 cm_storage；配置并构建，确认 cm_storage 生成。

**验收标准:**
- [ ] 单测可在无 MySQL 下构造 MysqlOptions 并调用 formatMysqlDescription。
- [ ] 输出含 "pool" 与计数，不含密码字符串。
- [ ] BUILD_STORAGE=OFF 时 cm_storage 目标不存在；ON 时生成。
- [ ] classifyMysqlError 对 2013/2006/2003/1213/1205/1062/9999 的映射与上表一致。

---

### Task 2.4：MysqlConn / PreparedStatement / Transaction

**Goal:** 用 RAII 管住连接、语句、事务三种资源，杜绝裸 new/delete 与漏回滚。

**Files:**
- Create: server/pool/mysql_conn.h
- Create: server/pool/mysql_conn.cpp
- Create: server/pool/prepared_statement.h
- Create: server/pool/prepared_statement.cpp
- Modify: server/pool/mysql_pool.h（新增 detail::PoolCore 定义）

**Steps:**
- [ ] mysql_conn.h 前向声明 cm::db::detail::PoolCore，并持有 std::shared_ptr<detail::PoolCore>；PoolCore 完整定义放 mysql_pool.h 的 detail 命名空间，mysql_conn.cpp include mysql_pool.h。
- [ ] 物理连接用 std::unique_ptr<MYSQL, MysqlCloser>；MysqlCloser::operator() 调 mysql_close。
- [ ] 建连参数：CONNECT/READ/WRITE timeout、MYSQL_OPT_RECONNECT=0、MYSQL_SET_CHARSET_NAME=utf8mb4、不启用 CLIENT_MULTI_STATEMENTS。
- [ ] 新连接执行 05 §4.3 的 6 条 SESSION 初始化语句；失败则关闭并计入 createFailTotal（计数由 PoolCore 提供）。
- [ ] PreparedStatement：两段式绑定（先收集值并 reserve，再填 MYSQL_BIND）、mysql_stmt_store_result、fetch 逐行、析构 mysql_stmt_close。
- [ ] Transaction：构造 BEGIN；valid() 反映成功；析构时未 commit 则 ROLLBACK；回滚失败把连接标记 broken；禁止嵌套（inTransaction 为真时 valid=false）。
- [ ] mysql_conn.h 必须 include mysql.h 才能声明 MYSQL；raw() 放 private，仅 friend MysqlPool/PreparedStatement 访问，DAO 只通过 prepare() 使用，不接触 MYSQL*。
- [ ] PreparedStatement 不拥有连接，生命周期必须短于 MysqlConn；调用方保证二者同作用域。

> 本任务的运行期行为（事务回滚、嵌套检测）在 Task 2.7 的集成用例中验证；2.4 结束时要求编译通过且无裸 new/delete。

**验收标准:**
- [ ] 全模块 grep 不到裸 delete / new（make_shared 除外）。
- [ ] 事务对象在作用域退出后，INSERT 的数据不可见（集成用例验证）。
- [ ] 嵌套 Transaction 的 valid() 为 false，且不发出第二条 BEGIN。

---

### Task 2.5：池获取、超时、慢查询

**Goal:** 让池在饱和与故障时快速失败，不把线程池拖死。

**Files:**
- Modify: server/pool/mysql_pool.cpp
- Modify: server/pool/mysql_pool.h

**Steps:**
- [ ] PoolCore 由 std::make_shared 创建，包含 mutex、condition_variable、idle deque、inUse 计数、waiters 计数、stopping 标志、失败计数；MysqlConn 析构调用 giveBack。
- [ ] 借出：pop idle → 空闲超过 30s 先 mysql_ping → 失败丢弃重建 → 成功刷新 lastActive；idle 为空且未超 maxSize 时新建，否则等待。
- [ ] 归还：broken 或 stopping 时 mysql_close；否则 push idle 并 notify_one。
- [ ] MysqlPool::create 校验 minSize>=1、maxSize>=minSize、maxSize<=32；非法返回 nullptr 并写 err。
- [ ] MysqlPool::describe() 调用 formatMysqlDescription，输出池计数。
- [ ] acquire(timeout)：循环「取 idle → 校验 → 未超 max 则新建 → 否则 wait_for」；超时返回 nullptr 并 acquireTimeoutTotal++。
- [ ] 连续 create 失败 3 次 → 熔断 5s；熔断期间 acquire 立即返回 nullptr，错误置 DbUnavailable。
- [ ] 后台 reaper 线程每 10s：关闭空闲超 idleTimeout 且总数 > minSize 的连接；关闭存活超 maxLifetime 的连接。
- [ ] 慢查询：steady_clock 包住 execute + 全部 fetch；超过 slowQueryMs 记 WARN（traceId/connId/costMs/rows/sql 占位符），slowQueryTotal++，更新 max。
- [ ] shutdown()：stopping=true、notify_all、等待 inUse 归零最多 5s、关闭 idle；reaper join。
- [ ] Debug 下 acquire 断言不在 EventLoop 线程（若可获取线程局部标记）；无法断言时至少在文档注释写明约束。

**验收标准:**
- [ ] maxSize=1 时，持有唯一连接后第二次 acquire(50ms) 在 200ms 内返回 nullptr，且 acquireTimeoutTotal=1。
- [ ] shutdown 后 acquire 立即返回 nullptr，不阻塞。
- [ ] slowQueryMs=10 时执行 SELECT SLEEP(0.05)，slowQueryTotal>=1，日志 SQL 不含参数值。
- [ ] 10 个线程并发 acquire/release 1000 次，无崩溃、无连接数超过 maxSize。
- [ ] 连接析构后，池的 inUse 计数回到 0，idle 不超过 maxSize。

> 本任务的运行期验收在 Task 2.7 执行；2.5 结束时要求编译通过。

---

### Task 2.6：最小 DAO

**Goal:** 用两条真实查询证明「池 + 预处理 + 事务」可以端到端工作，同时为 Phase 5 定下 DAO 形状。

**Files:**
- Create: server/db/user_dao.h
- Create: server/db/user_dao.cpp
- Create: server/db/file_dao.h
- Create: server/db/file_dao.cpp

**Steps:**
- [ ] UserDao::findByTel(MysqlConn&, const std::string& tel, UserRow* out)：返回 ErrorCode；密码字段只读出不打印。
- [ ] UserDao::insert(MysqlConn&, const UserRow&, uint64_t* id)：捕获 1062 → AlreadyExists。
- [ ] FileDao::findByMd5(MysqlConn&, const std::string& md5, FileRow* out)：返回 NotFound / DbQueryFailed。
- [ ] FileDao::insertOrAddRef(MysqlConn&, const FileRow&, uint64_t* id)：INSERT ... ON DUPLICATE KEY UPDATE f_ref_count=f_ref_count+1，新行 f_ref_count=1。
- [ ] DAO 不拼接 SQL，全部用 PreparedStatement；所有字符串参数按值传，不保存指针。

**验收标准:**
- [ ] 集成测试中 insert 两次同 md5：f_id 相同、f_ref_count=2。
- [ ] findByTel 对不存在手机号返回 NotFound，不抛异常。
- [ ] DAO 头文件不出现 MYSQL*（只接受 MysqlConn&）。
- [ ] UserRow::toString 或日志辅助函数不输出密码。

---

### Task 2.7：MysqlPool 测试

**Goal:** 建立「无 DB 单测 + 有 DB 集成」两档测试，并跑通 skip 约定。

**Files:**
- Create: server/tests/CMakeLists.txt
- Create: server/tests/storage/test_env.h
- Create: server/tests/storage/test_schema_file.cpp
- Create: server/tests/storage/test_mysql_unit.cpp
- Create: server/tests/storage/test_mysql_integration.cpp

**Steps:**
- [ ] test_env.h 提供 envFlag、envOr、mysqlEnabled、redisEnabled、mysqlOptions、redisOptions；不打印任何密码。
- [ ] 集成用例 SetUp 约定：
  - 环境变量未设置 → GTEST_SKIP()，消息说明需要设置哪个变量。
  - 环境变量已设置但连接失败 → FAIL()，不允许 skip。
  - schema 缺失 → FAIL()，提示先导入 sql/schema_v2.sql。
- [ ] server/CMakeLists.txt 在 BUILD_TESTS 且 BUILD_STORAGE 时 add_subdirectory(tests)；server/tests/CMakeLists.txt 创建 cm_storage_tests。
- [ ] target_compile_definitions(cm_storage_tests PRIVATE CM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")，供 SchemaFileTest 定位 sql/schema_v2.sql。
- [ ] CMake 注册两个 add_test：storage_unit（filter=-*Integration*）与 storage_integration（filter=*Integration*，仅 ENABLE_DB_INTEGRATION_TESTS=ON 时）；不通过 ENVIRONMENT 强行打开测试开关。
- [ ] 单测覆盖：schema 文件静态契约、Options 默认值/非法值、describe 脱敏、错误分类。
- [ ] 集成覆盖：连接 ping、预处理往返、事务回滚、提交持久化、获取超时、慢查询计数、重复键映射。

**验收标准:**
- [ ] 不设置任何环境变量时，ctest -R storage_unit 全绿，且没有用例被跳过（单测不应 skip）。
- [ ] 不设置环境变量时，storage_integration 未注册（若已注册则全部 skip，不计失败）。
- [ ] 设置 CM_TEST_MYSQL=1 后，storage_integration 全绿。
- [ ] 设置 CM_TEST_MYSQL=1 但端口错误时，用例 FAIL，不 skip。
- [ ] 测试输出与 CTest 日志中 grep 不到测试密码。

---

## Stage 3：RedisPool

### Task 3.1：RedisOptions、RedisReply 与 describe

**Goal:** 先落地可脱离 Redis 单测的纯逻辑。

**Files:**
- Create: server/pool/redis_pool.h
- Create: server/pool/redis_reply.h
- Create: server/pool/redis_reply.cpp

**Steps:**
- [ ] RedisOptions：host/port/password/db/connectTimeout/commandTimeout/minSize/maxSize/idleTimeout/acquireTimeout，默认 127.0.0.1:6379、db0、connect 200ms、command 300ms、min2/max8、idle60s、acquire200ms。
- [ ] RedisReply 持有 redisReply*，析构 freeReplyObject；提供 type()、isNil()、isError()、str()、integer()、elements()。
- [ ] 定义 RedisPoolStats（字段与 MysqlPoolStats 同构）。
- [ ] formatRedisDescription(const RedisOptions&, const RedisPoolStats&)：只输出 host/port/db 与计数，不输出 password。

**验收标准:**
- [ ] 手工构造 REDIS_REPLY_STRING/INTEGER/NIL/ERROR 四种 redisReply，读取结果正确，析构无泄漏（ASan 下跑）。
- [ ] describe 输出不含密码。
- [ ] Options 默认值与 05 §5.5 一致（min2/max8/200ms/300ms/60s/200ms）。

---

### Task 3.2：RedisConn 命令封装

**Goal:** 提供缓存与限流需要的命令封装与 Lua 入口，且保证连接出错即弃。

**Files:**
- Create: server/pool/redis_conn.h
- Create: server/pool/redis_conn.cpp

**Steps:**
- [ ] 连接建立：redisConnectWithTimeout + redisSetTimeout；有密码则 AUTH，db!=0 则 SELECT；任一步失败关闭连接。
- [ ] 命令全部用 redisCommandArgv，参数用 (const char*, size_t)；不做字符串拼接。
- [ ] 实现 get/set/setex/setPx/setNxPx/expire/del/hset/hget/incr/eval/ping；setPx 用毫秒 TTL，供短 TTL 测试。
- [ ] get/hget 返回 enum class Lookup { Hit, Miss, Error }，不把 Error 混成 Miss。
- [ ] eval(script, keys, args) 使用 argv 形式，keys 个数正确；脚本常量放 .cpp，不接收用户输入拼接。
- [ ] ctx->err 或 IO 超时 → broken=true；归还时销毁，不入 idle。
- [ ] 命令级 ERROR（reply->type==REDIS_REPLY_ERROR）→ 返回 false，连接仍可复用；WRONGTYPE 额外 DEL 坏键。
- [ ] 把 redis_reply.cpp、redis_conn.cpp 加入 cm_storage 源码列表并构建通过。

**验收标准:**
- [ ] 集成测试覆盖 SET/GET/DEL、SETEX 过期、SET NX PX、INCR+Lua TTL、compare-del。
- [ ] 读超时后用同一连接再次 GET 的行为是「连接被丢弃重建」，不会读到上一条回复。
- [ ] AUTH 失败时连接工厂返回失败，且错误信息不含密码。
- [ ] 连接上下文断开（重启 Redis）后，下一次 acquire 能重建并 PING 成功。

---

### Task 3.3：RedisPool 借还

**Goal:** 与 MysqlPool 同构的池化与退出顺序，避免析构竞争。

**Files:**
- Create: server/pool/redis_pool.cpp
- Modify: server/pool/redis_pool.h

**Steps:**
- [ ] RedisPool::create 校验 minSize>=1、maxSize>=minSize、maxSize<=32；非法返回 nullptr 并写 err。
- [ ] RedisCore 由 make_shared 创建；RedisConn 持有 shared_ptr<RedisCore>；析构 giveBack。
- [ ] 把 redis_pool.cpp 加入 cm_storage 源码列表并构建通过。
- [ ] acquire 超时 200ms，超时返回 nullptr 并计数；连续 3 次建连失败熔断 5s。
- [ ] 空闲超过 30s 的连接借出前 PING；失败销毁重建。
- [ ] shutdown 与 MysqlPool 同序：stopping、notify、等待 inUse、关闭 idle。
- [ ] reaper 每 10s 关闭 idle 超时连接，保留 minSize。

**验收标准:**
- [ ] maxSize=1 时第二次 acquire(50ms) 超时返回 nullptr。
- [ ] 8 线程并发借还 1000 次无崩溃，连接数不超过 maxSize。
- [ ] shutdown 后 acquire 立即失败。
- [ ] 非法 min/max 时 create 返回 nullptr 并写 err。
- [ ] 池内连接不会被两个线程同时持有（用线程 ID 断言或 ASan/TSan 跑一遍）。

---

### Task 3.4：RedisPool 测试

**Files:**
- Create: server/tests/storage/test_redis_unit.cpp
- Create: server/tests/storage/test_redis_integration.cpp
- Modify: server/tests/CMakeLists.txt

**Steps:**
- [ ] 单测：Options 默认值、非法池参数、describe 脱敏、RedisReply 四种类型。
- [ ] 构造测试用 redisReply 时用 calloc 分配 struct 与 str，交给 freeReplyObject 释放；不得指向字符串字面量（会被 free）。
- [ ] 集成：Ping、SetGetDel、SetNxPx 过期、INCR 首次设 TTL、compare-del 只删自己的锁、池耗尽超时。
- [ ] 所有 key 带 cm:test:<pid>:<testname>: 前缀，TearDown 删除；禁止 FLUSHDB。
- [ ] TTL 断言使用「100ms TTL + sleep 250ms」并允许一次重试，避免 CI 抖动。

**验收标准:**
- [ ] 无 Redis 时单测全绿，集成全 skip。
- [ ] 有 Redis 时集成全绿，重复运行 3 次无残留 key。
- [ ] 测试过程中不输出 AUTH 密码。

---

## Stage 4：缓存封装

### Task 4.1：CacheKey 与 TTL 抖动

**Goal:** 把 key 规范与抖动算法做成纯函数，先测边界再接 Redis。

**Files:**
- Create: server/cache/cache_key.h/.cpp
- Create: server/cache/cache_ttl.h/.cpp

**Steps:**
- [ ] CacheKey 提供静态函数，输出与 05 §6.1 完全一致：
  - session(token) → cm:v1:sess:{token}
  - user(uid) → cm:v1:usr:{uid}
  - fileByMd5(md5) → cm:v1:file:md5:{md5}
  - fileById(fid) → cm:v1:file:id:{fid}
  - share(code) → cm:v1:share:{code}
  - shareMiss(code) → cm:v1:share:miss:{code}
  - uploadBitmap(taskId) → cm:v1:up:bitmap:{taskId}
  - rateLimitIp(ip, window) → cm:v1:rl:ip:{ip}:{window}
  - rebuildLock(key) → cm:v1:lock:rebuild:{key}
  - downloadTicket(token) → cm:v1:dl:ticket:{token}
- [ ] 空参数/非法字符的防御：空字符串返回空 key；含空格或换行的 token 返回空 key（调用方拒绝）。
- [ ] jitterTtl(baseMs, randPercent) 为纯函数：result = base * (100 + randPercent) / 100，结果最小 1000ms；randPercent 由调用方保证在 [-percent, percent]。
- [ ] randomJitterTtl(baseMs, percent) 用 thread_local std::mt19937 + uniform_int_distribution(-percent, percent)。

**验收标准:**
- [ ] 每个 key 函数的输出与 05 §6.1 示例逐字符一致。
- [ ] percent=10 时，rand=-10/0/+10 分别得到 0.9x/x/1.1x。
- [ ] 10 万次随机采样全部落在 [0.9x, 1.1x]，且不同值数量 >=15（percent=10 时理论值 21 个）。
- [ ] base=1ms 时结果不小于 1000ms（下限保护）。

---

### Task 4.2：CacheStore 抽象与 Redis 适配

**Goal:** 让缓存逻辑可脱离 Redis 单测，同时保留真实实现。

**Files:**
- Create: server/cache/cache_store.h
- Create: server/cache/redis_cache_store.h/.cpp

**Steps:**
- [ ] 定义 enum class CacheLookup { Hit, Miss, Error }。
- [ ] 定义 CacheStore 纯虚接口：
  - get(key, value) → CacheLookup
  - setPx(key, value, ttlMs)：毫秒 TTL，底层 SET ... PX
  - setNxPx(key, value, ttl)
  - del(key)
  - evalDelIfEquals(key, token)
  - incrWithTtl(key, ttl, value)：Lua 原子 INCR + 首次 PEXPIRE，供限流使用
- [ ] RedisCacheStore 持有 RedisConn& 或 RedisPool&（二选一，按调用点决定；推荐 RedisPool& + 每次操作借还，避免长持有）。
- [ ] RedisConn::Lookup 到 CacheLookup 的翻译：Hit/Miss 原样，Error 不降级为 Miss。
- [ ] 不在 CacheStore 接口暴露 hiredis 类型。

**验收标准:**
- [ ] FakeCacheStore 可在单测中实现全部 5 个方法，不需要链接 Redis 服务。
- [ ] RedisCacheStore 的 Error 路径在单测中可用 Fake 模拟，CacheAside 行为可断言。
- [ ] 缓存头文件不 include hiredis.h（只有 redis_cache_store.cpp include）。

---

### Task 4.3：CacheAside 与 single-flight

**Goal:** 落地旁路缓存读路径、空值缓存、重建锁与回退。

**Files:**
- Create: server/cache/cache_aside.h/.cpp
- Create: server/cache/rebuild_guard.h/.cpp

**Steps:**
- [ ] 定义 CacheAsideOptions { std::chrono::milliseconds defaultTtl、nullTtl（默认 60000ms）、jitterPercent、waitMs（默认 200）、lockRetries（默认 3） }，测试可注入 200ms 短 TTL。
- [ ] 定义 enum class LoadStatus { Found, NotFound, Error } 与 typedef std::function<LoadStatus(std::string*)> Loader。
- [ ] CacheAside 构造接收 CacheStore& 与 CacheAsideOptions，提供 get(key, loader, out)：
  - GET Hit → 直接返回。
  - GET Miss → 进入 RebuildGuard。
  - GET Error → 直接调用 loader（降级），并计数。
  - loader 返回 NotFound → SET 空值标记（options.nullTtl，默认 60s），返回 Miss。
  - loader 返回 Error → 不写缓存，返回 Error。
  - loader 返回 Found → setPx 带抖动 TTL，返回 Hit。
  - GET Error 路径调用 loader 后不再尝试 setPx（Redis 已不可用），只返回结果并计数。
- [ ] RebuildGuard 本地单飞：unordered_map<string, shared_ptr<Slot>> + mutex + condition_variable；同 key 只允许一个 loader，其余等待最多 200ms。
- [ ] Redis 锁：SET NX PX 3000，token 为随机 32 hex；拿不到锁 sleep 20ms + 抖动重试最多 3 次；仍失败直接 loader（不返回错误）。
- [ ] 释放锁用 evalDelIfEquals，只删自己的 token。
- [ ] 计数器：cacheRebuildFallbackTotal、cacheLoadTotal、cacheHitTotal、cacheMissTotal，供 stats 读取。

**验收标准:**
- [ ] Hit 时 loader 调用次数为 0。
- [ ] Miss 时 loader 调用 1 次且缓存被写入。
- [ ] NotFound 时空值标记 TTL=60s，第二次读不再调用 loader。
- [ ] 8 线程同时读同一 miss key，loader 只调用 1 次，8 个返回值一致。
- [ ] 锁命令连续失败 3 次时，loader 仍被调用（回退），fallback 计数 +1。
- [ ] loader 返回 Error 时不写缓存，下一次读仍会重试。

---

### Task 4.4：限流器

**Goal:** 提供固定窗口限流，作为降级表中「不可降级」场景的执行器。

**Files:**
- Create: server/cache/rate_limiter.h/.cpp

**Steps:**
- [ ] RateLimitResult allow(const std::string& key, int limit, std::chrono::seconds window, int64_t* current)：通过 CacheStore::incrWithTtl 完成 INCR + 首次 PEXPIRE，返回 Allowed/Rejected/Unavailable 与当前计数；RateLimiter 不直接 include hiredis。
- [ ] key 由 CacheKey::rateLimitIp(ip, windowStart) 生成；windowStart = nowMs / windowMs。
- [ ] limit<=0 或 window<=0 时参数非法，直接返回 Rejected 并记 ERROR，不访问 Redis。
- [ ] Redis 错误时返回 Unavailable（与 Rejected 区分）；调用方按 05 §7 决定拒绝服务。
- [ ] 不把 Unavailable 当作 Allowed。

**验收标准:**
- [ ] limit=3 时前 3 次 Allowed，第 4 次 Rejected。
- [ ] 窗口过期后（sleep 窗口+100ms）重新 Allowed。
- [ ] Fake Store 返回 Error 时结果为 Unavailable，不是 Allowed。
- [ ] Lua 脚本不接受调用方传入的脚本文本，只接受 key/limit/window 参数。

---

### Task 4.5：缓存测试

**Files:**
- Create: server/tests/storage/test_cache_key.cpp
- Create: server/tests/storage/test_cache_ttl.cpp
- Create: server/tests/storage/test_cache_aside.cpp
- Create: server/tests/storage/test_cache_integration.cpp
- Modify: server/tests/CMakeLists.txt

**Steps:**
- [ ] FakeCacheStore 支持：预设 get 结果、记录 setPx/del 调用、模拟 setNxPx 失败、模拟 incrWithTtl/evalDelIfEquals 成功/失败。
- [ ] 把 cache/*.cpp 加入 cm_storage 源码列表并构建通过。
- [ ] 单测覆盖 Task 4.1/4.3/4.4 的验收点，不依赖 Redis。
- [ ] 集成覆盖：空值标记真实过期、锁真实互斥、限流真实窗口。
- [ ] 所有集成 key 带 cm:test:<pid>: 前缀并清理。

**验收标准:**
- [ ] 单测在无 Redis 时全绿。
- [ ] 集成在 Redis 可用时全绿，重复 3 次无残留。
- [ ] 单飞用例用 8 线程 × 20 轮，loader 总调用次数等于不同 key 数，不超过。

---

## Stage 5：集成验收与收尾

### Task 5.1：测试环境与 schema 契约

**Goal:** 把「无服务 skip、有服务必测」固化成可复用的测试夹具。

**Files:**
- Modify: server/tests/storage/test_env.h
- Create: server/tests/storage/test_schema_integration.cpp
- Modify: server/tests/CMakeLists.txt

**Steps:**
- [ ] test_env.h 提供：
  - bool mysqlEnabled() / redisEnabled()
  - MysqlOptions mysqlOptions() / RedisOptions redisOptions()
  - bool mysqlSchemaReady(MysqlPool&)：检查 7 张表是否都存在
- [ ] 集成夹具统一在 SetUp 中：
  - 未设置开关 → GTEST_SKIP，消息含需要设置的环境变量名。
  - 已设置但连接失败 → FAIL，输出 err（不含密码）。
  - schema 缺失 → FAIL，提示导入 sql/schema_v2.sql。
- [ ] schema 集成用例：information_schema 检查 7 张表、6 个唯一键、0 个外键、t_user_file 的 ROW_FORMAT=Dynamic。

**验收标准:**
- [ ] 不设环境变量时，所有 *Integration* 用例状态为 SKIPPED，不是 FAILED。
- [ ] 设置开关但 MySQL 未启动时，状态为 FAILED，错误信息可定位。
- [ ] schema 用例能在 cloud_media_test 上通过，且不修改任何数据。

---

### Task 5.2：MySQL / Redis 集成套件

**Goal:** 把 Stage 2/3 的集成用例补齐到可重复执行的程度。

**Files:**
- Modify: server/tests/storage/test_mysql_integration.cpp
- Modify: server/tests/storage/test_redis_integration.cpp

**Steps:**
- [ ] MySQL 用例使用唯一后缀（pid + 测试名 + 时间戳）生成手机号/MD5，TearDown 删除。
- [ ] 事务回滚用例：INSERT 后不 commit，析构后新连接查不到。
- [ ] 提交用例：commit 后新连接能查到，再删除。
- [ ] 获取超时用例使用独立 pool（maxSize=1），不影响其他用例。
- [ ] 重连用例：用第二个连接执行 KILL <id>，断言下一次 acquire 能重建；无 PROCESS 权限时 GTEST_SKIP 并说明。
- [ ] Redis 用例使用 db 15 与独立前缀；TTL 用例允许一次重试。

**验收标准:**
- [ ] 连续运行 3 次，无残留行、无残留 key。
- [ ] 并发用例（8 线程借还）无崩溃、无断言失败。
- [ ] KILL 用例在有权限时通过，无权限时明确 skip。
- [ ] 所有失败信息包含 SQLSTATE/errno 或 Redis 错误类型，便于定位。

---

### Task 5.3：缓存集成套件

**Files:**
- Modify: server/tests/storage/test_cache_integration.cpp

**Steps:**
- [ ] 空值标记：写 miss → GET 命中标记 → sleep 250ms（TTL 60s 的测试版可注入 200ms）→ 标记消失。
- [ ] 重建锁：两个 CacheAside 实例（模拟两个进程）同时 miss，各自持有 loader；总调用次数 <=2，返回值一致；若第二个在锁释放后进入，应命中缓存且总次数=1。
- [ ] 限流：limit=3，连续 4 次，前 3 Allowed、第 4 Rejected；窗口后恢复。
- [ ] Redis 不可用时：CacheStore 返回 Error，CacheAside 回退 loader，限流返回 Unavailable。

**验收标准:**
- [ ] 三个场景全部可重复通过。
- [ ] 锁释放后 key 不存在；持锁者异常退出时 3s 后锁自动过期（用 PTTL 断言）。
- [ ] 限流窗口边界不出现「第 4 次仍放行」。

---

### Task 5.4：全量验收

**Goal:** 按 05 的 DoD 完成一次可复现验收，并更新文档状态。

**Files:**
- Modify: docs/specs/05-存储设计.md（仅补「实施状态」一行，不改设计）
- Verify: 全仓库

**Steps:**
- [ ] 无服务单测：BUILD_STORAGE=ON、BUILD_TESTS=ON，运行 storage_unit。
- [ ] 有服务集成：导入 schema，导出 CM_TEST_*，运行 storage_integration。
- [ ] ASan：独立 build-asan 目录，ENABLE_ASAN=ON，跑 storage_unit + storage_integration。
- [ ] 依赖红线：common 无 Qt/FFmpeg；server 无 Qt；client 无 server 头。
- [ ] 密码检查：grep 源码/日志/CTest 输出，确认无测试密码。
- [ ] 文档：05 头部补「实施状态：Phase 4 完成（日期）」；不改任何设计结论。

**验收标准:**
- [ ] storage_unit 全绿，0 skipped。
- [ ] storage_integration 全绿，0 failed；skip 仅限 KILL 权限用例。
- [ ] ASan 无 leak/use-after-free 报告。
- [ ] grep 密码结果为 0。
- [ ] BUILD_STORAGE=OFF 时服务端仍可构建（无持久化警告）。

---

## gtest 清单与 GTEST_SKIP 约定

### 约定

1. 测试二进制只有一个：cm_storage_tests。
2. 用例名后缀 Integration 表示需要真实服务；CMake 用 --gtest_filter 分成两档。
3. 未设置 CM_TEST_MYSQL / CM_TEST_REDIS 时，Integration 用例在 SetUp 中调用 GTEST_SKIP()，状态为 SKIPPED。
4. 已设置开关但连接失败、schema 缺失、认证失败 → FAIL，不允许 skip。
5. 单项能力缺失（如 KILL 需要 PROCESS 权限）→ 在用例内部 GTEST_SKIP 并写明原因。
6. 测试不得打印密码、token、连接串；失败信息只含 host/port/db/errno。

~~~cpp
class MysqlIntegration : public ::testing::Test {
protected:
    void SetUp() override {
        if (!cm::test::envFlag("CM_TEST_MYSQL")) {
            GTEST_SKIP() << "set CM_TEST_MYSQL=1 to run MySQL integration tests";
        }
        std::string err;
        m_pool = cm::db::MysqlPool::create(cm::test::mysqlOptions(), &err);
        if (!m_pool) {
            FAIL() << "MySQL connection failed: " << err;   // 已开启开关，必须失败
        }
        if (!cm::test::mysqlSchemaReady(*m_pool)) {
            FAIL() << "schema missing: apply sql/schema_v2.sql to the test database";
        }
    }
    std::shared_ptr<cm::db::MysqlPool> m_pool;
};
~~~

### 用例清单

| 套件.用例 | 类型 | 断言 | 跳过条件 |
|---|---|---|---|
| SchemaFileTest.HasSevenTables | 单元 | 文件中 CREATE TABLE 计数=7，表名集合一致 | 无 |
| SchemaFileTest.HasRequiredUniqueKeys | 单元 | 含 uk_u_tel、uk_f_md5、uk_user_parent_name、uk_task_id、uk_s_code、uk_user_file | 无 |
| SchemaFileTest.HasNoForeignKeys | 单元 | FOREIGN KEY / REFERENCES 计数=0 | 无 |
| SchemaFileTest.HasNoTriggersOrViews | 单元 | CREATE TRIGGER / CREATE VIEW 计数=0 | 无 |
| StorageErrorCodeTest.ValuesAreUnique | 单元 | 4001~4008 不重复，且不与既有值冲突 | 无 |
| StorageErrorCodeTest.RetryabilityMatchesSpec | 单元 | is_retryable 与 05 §7.4 一致 | 无 |
| MysqlOptionsTest.DefaultsMatchSpec | 单元 | min2/max8/60s/500ms/30min/200ms | 无 |
| MysqlOptionsTest.RejectsInvalidPoolRange | 单元 | min=0、max<min、max>32 均失败 | 无 |
| MysqlDescribeTest.RedactsPassword | 单元 | 输出不含 password 值，含 host/port/db | 无 |
| MysqlErrorTest.ClassifiesConnectionErrors | 单元 | 2006/2013/2003 → DbUnavailable | 无 |
| MysqlErrorTest.ClassifiesDeadlockAndTimeout | 单元 | 1213/1205 → DbTxConflict | 无 |
| MysqlErrorTest.ClassifiesDuplicateKey | 单元 | 1062 → AlreadyExists | 无 |
| RedisOptionsTest.DefaultsMatchSpec | 单元 | 200ms/300ms/min2/max8/200ms | 无 |
| RedisDescribeTest.RedactsPassword | 单元 | 输出不含 password | 无 |
| RedisReplyTest.TypedAccessors | 单元 | STRING/INTEGER/NIL/ERROR 四类读取正确 | 无 |
| CacheKeyTest.MatchesSpecFormats | 单元 | 10 个 key 与 05 §6.1 一致 | 无 |
| CacheKeyTest.RejectsEmptyAndWhitespace | 单元 | 空/含空格/换行返回空 key | 无 |
| CacheTtlTest.Boundaries | 单元 | percent=10，-10/0/+10 → 0.9x/x/1.1x | 无 |
| CacheTtlTest.JitterDistribution | 单元 | 10 万次全在区间，不同值 >=15 | 无 |
| CacheTtlTest.MinimumOneSecond | 单元 | base=1ms 时结果 >=1000ms | 无 |
| CacheAsideTest.HitSkipsLoader | 单元 | loader 调用 0 次 | 无 |
| CacheAsideTest.MissLoadsAndFills | 单元 | loader 1 次，setPx 1 次 | 无 |
| CacheAsideTest.NotFoundWritesNullMarker | 单元 | 写 miss key，TTL=60s | 无 |
| CacheAsideTest.NullMarkerSkipsLoader | 单元 | 第二次读 loader 仍为 0 次 | 无 |
| CacheAsideTest.LoaderErrorDoesNotCache | 单元 | setPx 调用 0 次 | 无 |
| CacheAsideTest.StoreErrorFallsBackToLoader | 单元 | get=Error 时 loader 1 次 | 无 |
| CacheAsideTest.SingleFlightCallsLoaderOnce | 单元 | 8 线程，loader 1 次，结果一致 | 无 |
| CacheAsideTest.LockFailureRetriesThenLoads | 单元 | setNxPx 连续失败，loader 1 次，fallback=1 | 无 |
| RateLimiterTest.ParameterValidation | 单元 | limit<=0/window<=0 返回 Rejected | 无 |
| MysqlIntegration.ConnectAndPing | 集成 | SELECT 1 成功 | 未设 CM_TEST_MYSQL |
| MysqlIntegration.PreparedRoundTrip | 集成 | 插入后按手机号查回，字段一致 | 未设 CM_TEST_MYSQL |
| MysqlIntegration.TransactionRollbackOnDestruct | 集成 | 析构未 commit，新连接查不到 | 未设 CM_TEST_MYSQL |
| MysqlIntegration.CommitPersists | 集成 | commit 后新连接可查，清理成功 | 未设 CM_TEST_MYSQL |
| MysqlIntegration.AcquireTimeoutReturnsNull | 集成 | max=1，第二次 50ms 超时，计数=1 | 未设 CM_TEST_MYSQL |
| MysqlIntegration.SlowQueryIsCounted | 集成 | SLEEP(0.05) 后 slowTotal>=1 | 未设 CM_TEST_MYSQL |
| MysqlIntegration.DuplicateKeyMapsToBusinessError | 集成 | 1062 → AlreadyExists | 未设 CM_TEST_MYSQL |
| MysqlIntegration.ReconnectAfterKill | 集成 | KILL 后下一次 acquire 成功 | 未设 CM_TEST_MYSQL 或无 PROCESS 权限 |
| RedisIntegration.Ping | 集成 | PING 返回 PONG | 未设 CM_TEST_REDIS |
| RedisIntegration.SetGetDel | 集成 | 值一致，DEL 后 Miss | 未设 CM_TEST_REDIS |
| RedisIntegration.SetNxPxExpires | 集成 | 100ms 后 Miss；NX 第二次失败 | 未设 CM_TEST_REDIS |
| RedisIntegration.IncrSetsTtlOnce | 集成 | 首次 PTTL>0，第二次不重置 | 未设 CM_TEST_REDIS |
| RedisIntegration.CompareDelOnlyOwner | 集成 | 错误 token 删不掉，正确 token 可删 | 未设 CM_TEST_REDIS |
| RedisIntegration.PoolExhaustionTimesOut | 集成 | max=1，第二次 50ms 超时 | 未设 CM_TEST_REDIS |
| SchemaIntegration.SevenTablesExist | 集成 | information_schema 返回 7 | 未设 CM_TEST_MYSQL |
| SchemaIntegration.NoForeignKeys | 集成 | referential_constraints 计数=0 | 未设 CM_TEST_MYSQL |
| SchemaIntegration.UniqueKeysPresent | 集成 | 6 个唯一键 non_unique=0 | 未设 CM_TEST_MYSQL |
| CacheIntegration.NullMarkerExpires | 集成 | 注入 200ms TTL，过期后回 Miss | 未设 CM_TEST_REDIS |
| CacheIntegration.RebuildLockExclusive | 集成 | 两个实例并发，loader 总次数<=2 | 未设 CM_TEST_REDIS |
| RateLimiterIntegration.FixedWindow | 集成 | 3 放行，第 4 拒绝，窗口后恢复 | 未设 CM_TEST_REDIS |

## Verification

以下命令由人工执行；目标机 cmake 3.5 不支持 -S/-B 与 ctest --test-dir。

~~~bash
mkdir -p build-linux && cd build-linux
cmake .. -DBUILD_SERVER=ON -DBUILD_STORAGE=ON -DBUILD_TESTS=ON -DENABLE_DB_INTEGRATION_TESTS=ON
make -j8
~~~

无服务单测：

~~~bash
ctest -R storage_unit --output-on-failure
~~~

Expected: 100% tests passed；无 SKIPPED。

导入测试库（人工执行，密码交互输入）：

~~~bash
mysql -h 127.0.0.1 -u cm_test -p -e "source sql/create_database.sql"
mysql -h 127.0.0.1 -u cm_test -p cloud_media_test < sql/schema_v2.sql
~~~

集成测试：

~~~bash
export CM_TEST_MYSQL=1
export CM_TEST_MYSQL_HOST=127.0.0.1
export CM_TEST_MYSQL_PORT=3306
export CM_TEST_MYSQL_USER=cm_test
export CM_TEST_MYSQL_DB=cloud_media_test
export CM_TEST_MYSQL_PASSWORD='<人工输入>'
export CM_TEST_REDIS=1
export CM_TEST_REDIS_HOST=127.0.0.1
export CM_TEST_REDIS_PORT=6379
export CM_TEST_REDIS_DB=15
export CM_TEST_REDIS_PASSWORD='<人工输入或留空>'
ctest -R storage_integration --output-on-failure
~~~

Expected: 全绿；仅 KILL 用例在缺少 PROCESS 权限时 SKIPPED。

ASan：

~~~bash
mkdir -p build-asan && cd build-asan
cmake .. -DBUILD_SERVER=ON -DBUILD_STORAGE=ON -DBUILD_TESTS=ON -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
make -j8
ctest -R storage --output-on-failure
~~~

Expected: 无 AddressSanitizer/UBSan 报告。

构建回退验证：

~~~bash
cmake .. -DBUILD_SERVER=ON -DBUILD_STORAGE=OFF -DBUILD_TESTS=ON
make -j8
~~~

Expected: 成功；配置阶段出现「server will run without persistence」WARNING。

## Rollback

- 代码全部为新增文件 + 少量修改（options、thirdparty、server/CMakeLists、error_code、test_error_code、tests/CMakeLists），回滚 = 还原这些修改并删除新增文件。
- 数据库：本阶段只操作 cloud_media_test；回滚可直接 DROP DATABASE cloud_media_test。生产库 cloud_media 在本阶段不写入。
- 测试环境变量只影响当前 shell；关闭终端即失效。
- 若 CMake 依赖定位失败导致构建阻塞，可临时用 -DBUILD_STORAGE=OFF 恢复服务端构建；不得通过提交硬编码路径绕过。

## Risks

| 风险 | 触发信号 | 影响 | 对策 |
|---|---|---|---|
| MySQL 5.6 或 compact 行格式 | 建表报 1071 key too long | t_user_file 无法创建 | 配置阶段/启动自检 @@innodb_large_prefix；锁定 5.7；不改索引设计 |
| hiredis 0.13 无 redisReconnect | 编译报未声明 | RedisPool 无法编译 | 统一 redisFree + redisConnectWithTimeout 重建，禁止调用 redisReconnect |
| CMake 3.5 能力不足 | 配置报 unknown command | 构建失败 | 只用 find_path/find_library/UNKNOWN IMPORTED/separate_arguments；禁止 target_link_options/FetchContent |
| 集成测试互相污染 | 重复运行出现 1062/残留 key | 用例偶发失败 | 唯一后缀 + TearDown 精确删除；禁止 FLUSHDB；禁止写生产库 |
| TTL/超时用例在 CI 抖动 | 100ms TTL 偶发未过期 | 假失败 | 客户端 sleep ≥1.5×TTL；允许一次重试；不用 10ms 级 TTL |
| 在 IO 线程 acquire | 压测时事件循环卡死 | P99 飙升 | Debug 断言 + 代码评审；连接池只在业务线程使用 |
| 密码进入日志/测试输出 | grep 命中 password 值 | 安全事故 | 只从环境变量读；describe 脱敏；单测断言；CI 不用明文变量 |
| MySQL wait_timeout 短于 max_lifetime | 借出后第一条 SQL 报 2013 | 偶发查询失败 | 30s 空闲 ping；max_lifetime=30min；连接级错误丢弃重建 |
| 静态 libmysqlclient | 链接报未定义 ssl/crypto | 链接失败 | 优先共享库；必要时把 mysql_config --libs 的依赖挂到 INTERFACE_LINK_LIBRARIES |
| 测试需要 KILL 权限 | 2013/1095 或权限错误 | 重连用例失败 | 用例内检测权限并 GTEST_SKIP，不计失败 |
| 无网络环境缺依赖 | 配置报找不到 mysql.h/hiredis.h | 阶段阻塞 | 由人工预先 apt 安装；计划不提供自动下载 |

## Deferred Decisions

- TransferDao / ShareDao / PlayDao：Phase 5 实现；本阶段只用 UserDao/FileDao 验证池 API。安全默认：业务代码不得绕过 DAO 直接拼 SQL。
- PreparedStatement 缓存：本阶段每次 prepare；若 Phase 7 压测显示占比高，再加连接内 LRU。
- Redis pipeline / Sentinel / Cluster：本阶段不做；接口按单实例设计，未来加 Sentinel 不改调用方。
- MySQL 8.0：本阶段锁定 5.7；若升级，需重新验证 DATETIME(6)、索引长度与 max_execution_time 行为。
- 密码算法升级 bcrypt/Argon2：属于 Phase 5 认证设计；本阶段只预留 u_pwd_algo 字段。
- 直链 token 的签名算法：Phase 5 决定；本阶段只提供 Redis TTL 存储原语。
