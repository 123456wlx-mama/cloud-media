# Phase 5 零拷贝门槛记录（H1）

**日期：** 2026-10-19
**状态：** PASS — 用户确认 ctest --output-on-failure 全绿
**测试文件：** common/tests/test_crc32_combine.cpp
**被测函数：** cm::crc32Combine(crc1, crc2, len2)

## 对拍设计

| 项 | 内容 |
|---|---|
| 组数 | 128 组（计划要求 ≥100） |
| 随机种子 | 0xC0FFEE |
| 固定长度 | 0、1、3、4MB-1、4MB、1024、65535、2 |
| 随机长度 | 其余组 0~256KB |
| 切分点 | 每组随机 0~len |
| 基准 | 同一段 A||B 用 cm::Crc32 流式 update，与 combine(crc32(A), crc32(B), len(B)) 比较 |
| 失败信息 | case / lenA / lenB / expected / actual |

## 验收命令（人工在 Linux 构建目录执行）

~~~bash
cmake --build build -j8
ctest --test-dir build -R crc32_combine --output-on-failure
~~~

目标机 cmake 3.5 不支持 --test-dir，改用：

~~~bash
cd build
ctest -R crc32_combine --output-on-failure
~~~

## 结论

**PASS。** 用户确认在 VM 上执行 `ctest --output-on-failure` 全绿；`crc32_combine` 的 128 组对拍包含在本次全量 ctest 中。

因此：

- H1 门槛通过，允许在 D-0 中把 `CM_DOWNLOAD_ZEROCOPY` 的默认值翻到 ON。
- 本记录依据用户确认的结果；原始 ctest 输出由用户保留。
- 若后续修改 `crc32Combine` 或 `Crc32`，必须重新执行 128 组对拍，并重新评估默认值。

## 待补记录

执行后在此追加：

- [ ] 构建配置与 commit
- [ ] ctest 输出摘要（128 组）
- [ ] 失败用例（如有）
- [ ] 最终结论 PASS / FAIL
