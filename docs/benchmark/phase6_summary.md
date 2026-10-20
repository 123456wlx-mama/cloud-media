# Phase 6 总验收（T6-4 ~ T6-7，2026-10-20）

## DoD 对照

| 指标 | 目标 | 实际 | 结论 |
|---|---|---|---|
| 单元/集成测试用例数 | ≥ 60 | **346** 个 gtest 用例（43 个 ctest 注册项） | 通过 |
| `common/` 行覆盖 | ≥ 80% | **91.0%**（T6-2 补测后） | 通过 |
| `server/net/` 行覆盖 | ≥ 60% | **83.2%** | 通过 |
| 全量 ctest | 全绿 | **43/43 Passed** | 通过 |
| Codec/Buffer Fuzz | 1M 随机输入无崩溃 | **1,000,000 迭代，ASan 0 报错**（T6-6） | 通过 |
| CI | 脚本 + workflow，每次提交自动跑 | **GitHub Actions 首跑全绿**（run 39370428189，2m19s）：cpp11 红线 ALL CLEAN + 43/43 + common 92.3% / net 83.7% | 通过 |

> 用例数说明：355 是早期估算；按 `--gtest_list_tests` 统计当前 ctest 实际注册的 17 个 gtest 可执行，共 346 例。
> `build/bin` 里另有 5 个未注册的旧 DAO 可执行（28 例），属于历史残留，不计入。

## 本阶段提交清单（按任务）

| 任务 | 产出 |
|---|---|
| T6-1 | `docs/benchmark/phase6_coverage_baseline.md`；`build-cov` 覆盖率基线（common 89.0%、net 83.1%） |
| T6-2 | 22 个新用例：ErrorCode +3、ServerConfig +4、IniConfig +3、CodecBulk +7、Buffer +5；`handler_registry` 原有 7 例已覆盖，未重复 |
| T6-3 | `scripts/check_cpp11.sh`（含裸 new）；`NULL → nullptr` 412 处 / 75 文件；新增 `wire_code_to_error`；`docs/benchmark/phase6_cpp11_check.md` |
| T6-4 | `scripts/ci.sh`；`.github/workflows/ci.yml` |
| T6-5 | 本文 |
| T6-6 | `common/protocol/tests/fuzz_codec_driver.cpp` + `cm_fuzz_driver` target；`docs/benchmark/phase6_fuzz.md`（1M 迭代 ASan 0 报错） |
| T6-7 | CI 首跑打通：MySQL 改服务容器、补 `default-mysql-client`、新增 `server/db/mysql_compat.h`（my_bool 8.0 兼容）；run 39370428189 全绿 |

## 覆盖率复测（T6-2 后）

| 范围 | 行覆盖 | 函数覆盖 |
|---|---|---|
| `common/` | 91.0% | 98%+ |
| `server/net/` | 83.2% | 90%+ |

最低的 15 个生产源文件（`lcov LH/LF`，排除 tests）：

| # | 文件 | 行覆盖 | 归类 |
|---|---|---|---|
| 1 | server/net/Poller.cpp | 57.1% | 不可测为主（抽象基类/平台） |
| 2 | server/net/TcpServer.cpp | 58.9% | 测试盲区（生命周期） |
| 3 | server/net/Socket.cpp | 60.7% | 不可测为主（系统调用失败） |
| 4 | server/net/TcpConnection.cpp | 65.2% | 资源错误路径 + 状态机盲区 |
| 5 | server/net/handler_registry.cpp | 74.7% | 准入/停机路径 |
| 6 | server/net/Acceptor.cpp | 79.4% | 不可测为主（EMFILE/ABORTED） |
| 7 | common/config/server_config.cpp | 81.6% | 校验分支（部分已补） |
| 8 | common/base/buffer.cpp | 81.8% | readFd errno（已补 EAGAIN/EINTR） |
| 9 | common/config/ini_config.cpp | 84.3% | 解析边界 |
| 10 | common/log/logger.cpp | 85.6% | 滚动/磁盘写失败（资源） |
| 11 | server/net/EpollPoller.cpp | 86.8% | 不可测为主（epoll 异常） |
| 12 | common/protocol/codec.cpp | 87.5% | decodeBulk 其余边界 |
| 13 | server/net/Channel.cpp | 89.7% | 事件位组合 |
| 14 | server/net/TimerQueue.cpp | 90.4% | timerfd 边界（平台） |
| 15 | server/net/EventLoop.cpp | 94.9% | 唤醒/退出路径 |

`common/base/error_code.cpp` 已由 55.6% 提升到 100%（T6-2 全表覆盖）。

**「不可测」豁免**：`Poller` 抽象/平台分支、`Socket`/`Acceptor` 的 bind/listen/accept 失败、
`EpollPoller` 的 epoll_ctl 异常、`TimerQueue` 的 timerfd 边界，需要故障注入或平台桩，
本轮按基线结论保留，不强行补测。

## C++11 红线

| 项 | 结果 |
|---|---|
| `NULL` | 412 → 0（75 个文件，代码区替换；SQL/字符串未动） |
| 裸 `delete` | 0（4 处为 CLI 字符串子命令，脚本排除） |
| `volatile` | 0 |
| `memset(&结构体)` | 17 处均为 C API 结构体，脚本白名单豁免 |
| 裸 `new` | 55 处均为 `unique_ptr/shared_ptr` 构造，过滤后 0 |
| C++14/17 特性 | 0 |

## CI

`scripts/ci.sh`（VM 实测）：

1. `scripts/check_cpp11.sh` → 全部 clean；
2. 重新配置并构建 `build-cov`（`ENABLE_COVERAGE=ON`、`ENABLE_DB_INTEGRATION_TESTS=ON`）；
3. `ctest`（MySQL/Redis 环境变量来自 env）→ **43/43 Passed**；
4. lcov 采集 + 阈值断言 → common 91.0% ≥ 80、net 83.2% ≥ 60；
5. 输出 `CI OK`，退出码 0。

`.github/workflows/ci.yml` 要点：

- `ubuntu-22.04`，apt 安装 `cmake g++ make lcov redis-server default-mysql-client default-libmysqlclient-dev libhiredis-dev`；
- **MySQL 用服务容器**（`services.mysql`，`mysql:8.0`，`MYSQL_ROOT_PASSWORD` + `MYSQL_DATABASE` + `mysqladmin ping` health check），
  不用 runner 预装 MySQL——其 root 非 auth_socket、口令未知，`sudo mysql` 会 Access denied；
- init 步骤：root 建 `cm_test`（`mysql_native_password`），导入 `sql/schema_v2.sql` + 两个 patch；
- `cmake ... -DCMAKE_CXX_STANDARD=11 -DCMAKE_CXX_EXTENSIONS=OFF -DENABLE_COVERAGE=ON -DENABLE_DB_INTEGRATION_TESTS=ON`；
- 实际构建/测试/覆盖率统一调用 `bash scripts/ci.sh`，避免 workflow 与本地脚本漂移；
- 覆盖率阈值在 `ci.sh` 内断言（common ≥80、server/net ≥60）。

### 首跑记录（T6-7，run 39370428189）

前 7 次失败逐一定位：①runner 预装 MySQL root 非 auth_socket（`ERROR 1045`，`::error::` 注解抓到）→
②改 `mysqld --init-file` 后台常驻不可靠 → ③改服务容器但漏装 `mysql` CLI → ④补 CLI 后
暴露真正的编译问题：**MySQL 8.0 客户端移除 `my_bool`**。最终以 `server/db/mysql_compat.h`
（8.0 恢复 1 字节别名 + `cmBoolPtr()` 转换 `MYSQL_BIND.is_null/.error`）双兼容 5.7/8.0，
run 39370428189 全绿：ALL CLEAN + 43/43 + common 92.3% / net 83.7% + CI OK（2m19s）。

**g++11 vs g++5.4 差异记录**（此前标记「待首次推送验证」）：

- 无编译错误差异；唯一硬错误是 `my_bool`（属 MySQL 8.0 客户端头文件差异，非编译器差异），已由 `mysql_compat.h` 解决；
- 警告无新增阻断项（项目未开 `-Werror`）；覆盖率略有出入属编译器版本正常波动（common 91.0%→92.3%、net 83.2%→83.7%，均为 g++11 侧数据）；
- `Node.js 20 deprecation` 警告来自 `actions/checkout@v4`，与项目代码无关。

## 遗留与风险

1. **分支覆盖未采集**：lcov 报 `branches...: no data found`，当前 `--coverage` 只产出行/函数数据。
2. `build/bin` 存在 5 个历史 DAO 可执行（未注册 ctest），建议后续清理构建目录，不影响当前 43 项。
3. `actions/checkout@v4` 的 Node.js 20 弃用警告：升级到 v5 可消除，非功能问题。
