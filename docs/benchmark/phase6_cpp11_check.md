# Phase 6 C++11 红线自检（T6-3，2026-10-19）

## 脚本

`scripts/check_cpp11.sh`：在路线图 9.4 的五项检查上增加「裸 new」。
只扫描 `common/` 与 `server/`；`legacy/`、`build*/`、`thirdparty/` 不在范围。
任何命中都会让脚本以非 0 退出。

检查项：

1. 裸 `delete`（排除 `= delete`；排除 CLI 字符串里的 `"delete"` 子命令）
2. `volatile`
3. `memset(&结构体)`
4. `NULL`（先剥离字符串与行注释，避免把 SQL 的 `IS NULL` 误判）
5. `std::optional` / `string_view` / `std::filesystem` / `std::make_unique`
6. 裸 `new`（排除 `unique_ptr` / `shared_ptr` / `reset(` / `make_shared` / `makeUnique` 包装）

## 已知豁免（C API 兼容）

- `memset(&MYSQL_BIND, ...)`、`struct tm`、`sockaddr`、`addrinfo`、`sigaction`：
  这些是 C 库结构体，C++11 下仍必须零初始化；脚本按接收者白名单排除，
  项目自有结构体（`VideoState`、任务行等）不在豁免内。
- `NULL` 检查剥离字符串和注释：SQL 里的 `IS NULL`/`= NULL` 不会被误报。
- `unique_ptr<T>(new T(...))` 是 C++11 允许的写法（项目禁用 C++14 的 `make_unique`），
  脚本只拦截没有被智能指针包装的 `new`。

## 自检前后

| 项 | 修改前 | 修改后 |
|---|---|---|
| C++ 代码里的 `NULL` | 412 处 / 75 个文件 | **0**（全部改为 `nullptr`，字符串与注释未动） |
| 裸 `delete` | 0（4 处是 CLI 字符串里的 `delete` 子命令） | 0 |
| `volatile` | 0 | 0 |
| `memset(&C 结构体)` | 17 处，均为 C API 零初始化 | 17（脚本白名单豁免） |
| 裸 `new` | 55 处，全部为 `unique_ptr/shared_ptr` 构造 | 0（脚本过滤后） |
| C++14/17 特性 | 0（仅 `memory.h` 注释提到 `make_unique`） | 0 |

`NULL → nullptr` 由一次性 tokenizer 脚本完成：只替换代码区，跳过字符串、字符字面量与注释，
因此 SQL 语句、日志文案与文档字符串保持不变。

## 生产代码的唯一非机械改动

新增 `cm::wire_code_to_error(std::uint32_t)`（`common/include/cm/base/error_code.h` + `common/base/error_code.cpp`），
用于 T6-2 的 `to_wire_code → wire_code_to_error` 双向断言。语义：

- `0 → Ok`，未知 code → `UnknownError`；
- `3002` 歧义：`to_wire_code(TaskNotFound)` 折成 `ChunkMissing`，反向只能还原为 `ChunkMissing`（测试中显式断言）。

## 测试结果

- 新增 22 例：ErrorCode +3、ServerConfig +4、IniConfig +3、CodecBulk +7、Buffer +5。
- `cm_common_tests` 目标相关 37 例全绿；`handler_registry` 7 例全绿（准入/释放/未知类型/重复注册原本已覆盖，未重复添加）。
- 全量 `ctest`：**43/43 Passed，22.45s**（`CM_TEST_MYSQL=1 CM_TEST_REDIS=1`）。
- 构建：`make -j4` 零 warning。

## 脚本输出

```text
== 裸 delete（排除 = delete 与字符串） ==
clean
== volatile ==
clean
== memset(&C 结构体) ... ==
clean
== NULL（已剥离字符串与行注释） ==
clean
== C++14/17 越界特性 ==
clean
== 裸 new ... ==
clean
ALL CLEAN
```
