# Phase 6 Fuzz 验证（T6-6，2026-10-20）

> 对应 `docs/specs/02-协议设计.md` 验收项与 `docs/plans/phase-2.md` Deferred：
> 「fuzz 在 Phase 6 补 1M 随机输入无崩溃」。

## 环境

| 项 | 值 |
|---|---|
| OS / 编译器 | Ubuntu 16.04 / g++ 5.4.0 |
| 构建目录 | `build-asan`（`-DENABLE_ASAN=ON`，ASan + UBSan） |
| 目标 | `cm_fuzz_driver`（独立可执行，非 gtest，不注册 ctest） |
| 源码 | `common/protocol/tests/fuzz_codec_driver.cpp` |

## 驱动设计

三类输入混合（确定性 PRNG xorshift64\*，固定种子，可复现）：

| 权重 | 输入类型 | 覆盖路径 |
|---|---|---|
| 50% | 结构化变异 | 从合法帧出发做截断/字节翻转/长度字段/类型字段/版本/魔数篡改/追加粘包，命中 CRC、类型、长度等深层校验分支 |
| 35% | 纯随机字节（0~64KB） | 魔数/版本/长度第一道防线 + `decodeBulk` |
| 15% | Buffer 操作 | 随机 append/retrieve/retrieveAsString/shrinkTo/reset，校验 peek 与 readableBytes 不变量 |

每轮同时喂 `Codec::decode` 与 `Codec::decodeBulk`，且一半走「分块流式喂入」（覆盖半包 NeedMore 与粘包路径）。

## 运行结果

```text
fuzz: 1000000 iterations, seed fixed
  ... 100000 / 1000000
  ...
  ... 1000000 / 1000000
FUZZ OK: ok=90529 needMore=1162958 protoErr=289449819 (no crash)
```

- 迭代：**1,000,000**；退出码 **0**。
- ASan/UBSan 报错：**0**（无 `AddressSanitizer` / `runtime error:` / `SUMMARY`）。
- 状态分布说明深层分支确实被覆盖：`ok=90529`（CRC/类型校验通过 9 万次）、`needMore=1162958`（半包路径）、`protoErr=289449819`（魔数/版本/长度/CRC/未知类型错误，含随机输入与流式分块产生的海量解码尝试）。

## 复现命令

```bash
# VM：build-asan 已 -DENABLE_ASAN=ON
cd ~/cloud-media/build-asan && cmake .. && make cm_fuzz_driver -j4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ./bin/cm_fuzz_driver 1000000
```

迭代数可用参数覆盖（如 `./bin/cm_fuzz_driver 100000`）。

## 结论

**通过。** 1M 随机输入（含结构化变异 + 纯随机 + Buffer 操作）下 `Codec::decode` / `decodeBulk` / `Buffer` 无崩溃、无越界、无内存错误。

> 注：`cm_fuzz_driver` 作为编译目标纳入构建（防源码漂移），但不注册进 ctest，避免拖慢常规 CI；如需纳入回归可后续以 10k 迭代登记一个 smoke 用例。
