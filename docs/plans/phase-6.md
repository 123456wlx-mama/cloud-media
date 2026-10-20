# Phase 6 实施计划（测试体系：GoogleTest + 集成 + 覆盖率 + Fuzz + CI）

> 对应《终端改造路线图》Phase 6。本文件是计划与任务分解；完成数据见
> `docs/benchmark/phase6_summary.md`（总验收）、`phase6_coverage_baseline.md`（T6-1）、
> `phase6_cpp11_check.md`（T6-3）、`phase6_fuzz.md`（T6-6）。

## Overview

把「我有测试」变成可量化证据：单元/集成测试 ≥60 例、行覆盖 `common/` ≥80% 与
`server/net/` ≥60%、C++11 红线清零、Codec/Buffer fuzz 1M 无崩溃、CI 每次提交自动跑。

## DoD（全部达标后才算收口）

| 指标 | 目标 |
|---|---|
| gtest 用例数 | ≥ 60 |
| 行覆盖 | `common/` ≥ 80%、`server/net/` ≥ 60% |
| 全量 ctest | 全绿（含 MySQL/Redis 集成用例） |
| C++11 红线 | 裸 delete / volatile / memset(&结构体) / NULL / C++14-17 特性 / 裸 new 全部清零 |
| Fuzz | 1M 随机输入无崩溃（ASan/UBSan） |
| CI | 每次提交自动跑，脚本 + workflow 双侧一致 |

## Files and Responsibilities

| 文件 | 职责 |
|---|---|
| `common/tests/*`、`common/protocol/tests/*` | common 层 gtest（Buffer/CRC32/ErrorCode/IniConfig/ServerConfig/Logger/MD5/Codec/序列化/业务消息） |
| `common/protocol/tests/fuzz_codec_driver.cpp` | Fuzz 驱动（独立可执行 `cm_fuzz_driver`，不注册 ctest） |
| `server/net/tests/*`、`server/tests/**` | net / logic / storage gtest 与集成测试 |
| `scripts/check_cpp11.sh` | C++11 红线自检（六项，命中即非 0 退出） |
| `scripts/ci.sh` | 一键 CI：红线 + 构建 + 全量 ctest + lcov 覆盖率阈值断言 |
| `.github/workflows/ci.yml` | GitHub Actions：MySQL 服务容器 + Redis + 调 `ci.sh` |
| `server/db/mysql_compat.h` | MySQL 5.7 / 8.0 客户端双兼容（my_bool 移除问题） |
| `docs/benchmark/phase6_*.md` | 各任务证据与总验收 |

## Task Map（严格按此顺序）

### Task T6-1：覆盖率基线
`build-cov`（`-DENABLE_COVERAGE=ON`）+ lcov capture/extract；产出
`phase6_coverage_baseline.md`。注意 lcov 1.12 的 `--no-external` 在本布局失效，
须先全量 capture 再按绝对路径 extract。

### Task T6-2：盲区补测
对照 T6-1 的低覆盖文件补边界用例（ErrorCode 全表、ServerConfig/IniConfig 校验分支、
CodecBulk 边界、Buffer readFd errno），目标 common ≥80%。

### Task T6-3：C++11 红线清零
`scripts/check_cpp11.sh`（在路线图 9.4 五项上增加裸 new；NULL 检查剥离字符串/注释，
豁免 C API 结构体 memset）；`NULL → nullptr` 全量替换；结果回填 `phase6_cpp11_check.md`。

### Task T6-4：CI 脚本 + workflow
`scripts/ci.sh` 单一事实源（阈值断言只写在这一处）；workflow 统一调用它，避免漂移。

### Task T6-5：总验收文档
DoD 逐项对照 + 不可测豁免清单 + 遗留风险。

### Task T6-6：Fuzz（补 phase-2/specs-02 的 Deferred 欠账）
`cm_fuzz_driver`：确定性种子（可复现），三类输入——结构化变异（打 CRC/类型/长度深层
分支）+ 纯随机（0~64KB）+ Buffer 操作不变量；`decode`/`decodeBulk` 双入口、一半走分块
流式喂入覆盖半包/粘包。`build-asan`（ASan+UBSan）跑 1M 迭代，0 报错为通过。
不注册 ctest（长跑），如需回归可后续加 10k 迭代 smoke 用例。

### Task T6-7：CI 首跑打通
要求：push 触发、全绿、无跳步。已知坑（全部实测踩过）：

1. runner 预装 MySQL 的 root **非 auth_socket** 且口令未知，`sudo mysql` 必然
   `ERROR 1045` → **必须用 `services:` MySQL 服务容器**（root 口令已知 + health check），
   不要在 runner 上起系统 MySQL；
2. 服务容器方案下 apt 只装 `default-libmysqlclient-dev` 没有可执行 `mysql` → 补
   `default-mysql-client`；
3. MySQL 8.0 客户端头文件移除 `my_bool`（`MYSQL_BIND.is_null/.error` 变 `bool*`）→
   `server/db/mysql_compat.h`：8.0 恢复 1 字节别名 + `cmBoolPtr()` 转换；
   存储保持 1 字节（禁止 `std::vector<bool>` 位压缩）；VM 5.7 与 CI 8.0 双侧编译验证。

## Verification

~~~bash
# VM（5.7 侧）
bash scripts/ci.sh                     # ALL CLEAN + 43/43 + 覆盖率门槛 + CI OK
./build-asan/bin/cm_fuzz_driver 1000000   # FUZZ OK (no crash)

# CI（8.0 侧，push 触发）
gh run list --limit 1                  # conclusion=success
~~~

首跑通过记录：run 39370428189（2m19s）：ALL CLEAN、43/43、common 92.3% / net 83.7%、CI OK。

## Rollback

- `mysql_compat.h` 是纯增量兼容层，回退即删除并恢复三处 `#include <mysql.h>`（8.0 客户端下将无法编译，仅 5.7 可用）。
- CI 回退：删 `.github/workflows/ci.yml` 即可，`scripts/ci.sh` 在 VM 上独立可用。

## Blocking Questions

无。

## Deferred Decisions

- 分支覆盖率：g++5.4 `-fprofile-arcs` 不产分支数据，lcov 报 `no data found`；DoD 只要求行覆盖，暂不引入 gcovr/新工具链。
- `cm_fuzz_driver` 进 ctest 常驻 smoke（10k 迭代）：待 Phase 7 压测阶段一起权衡 CI 时长。
- `actions/checkout@v4` 的 Node.js 20 弃用警告：升级 v5 属依赖维护，不影响功能。
- `build/bin` 历史 DAO 可执行残留：属构建目录清理，不入库处理。
