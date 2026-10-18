# C++11 项目规范（cloud-media）

> 来源：终端改造路线图 9.1、9.2（原文）
> 适用范围：common/、server/、client/ 的新增与改动代码；legacy/ 为只读归档，不适用。
> 执行方式：每个模块迁移或新增代码都必须满足本规范；阶段验收时逐条检查。

## 9.1 规范要求

| 主题 | 要求 |
|---|---|
| 资源管理 | 一律 RAII：`unique_ptr` 独占、`shared_ptr` 共享；文件/MySQL/Redis 连接用析构自动释放；**禁用裸 `new/delete` 管资源** |
| 并发 | `std::thread` / `std::mutex` / `std::condition_variable` / `std::atomic`（状态标志禁用 `volatile`） |
| 时间 | `std::chrono`（超时、定时器、耗时统计），不用 `gettimeofday` 裸调 |
| 语法 | `auto`、范围 for、`nullptr`、`override`/`final`、`=delete`、`enum class`、`using` 别名、`constexpr` |
| 性能 | 移动语义（大 Buffer 传值改 `std::move`）、`emplace_back`、完美转发 |
| 回调 | `std::function` + `std::bind` + lambda（替代函数指针数组 `PFUN`） |
| **禁用** | 不引入 C++14/17 特性（`std::optional`、`string_view`、`std::filesystem`、泛型 lambda），保持纯 C++11 |
| 编译 | `-std=c++11 -Wall -Wextra`，Debug 开 ASan/UBSan，Release 开 `-O2` |

## 9.2 现有代码 → C++11 对照表（照着改就行）

| 你现在的代码 | 改成 | 出现位置 |
|---|---|---|
| `char* buf = new char[len]; ... delete[] buf;` | `std::vector<char> buf(len);` / `std::unique_ptr<char[]>` | 收发缓冲、文件读写 |
| `memset(&videoState, 0, sizeof(...))` | 构造函数初始化列表（你已经在 `VideoState` 踩过这个坑） | `VideoState`、协议结构体 |
| `volatile bool quit;` | `std::atomic<bool> quit;` | 播放器、服务端退出标志 |
| 函数指针数组 `PFUN[]` 做协议分发 | `std::unordered_map<int, std::function<...>>` + lambda 注册 | `CKernel` 协议分发 |
| `pthread_create` / `SDL_CreateThread` 裸用 | `std::thread`（播放器侧；Linux 服务端本就用 pthread 池可保留） | 播放器视频线程 |
| `MYSQL* conn` 手动开关 | RAII：`MysqlConn` 析构自动归还连接池 | 服务端全部 DB 调用 |
| `gettimeofday` / 手写时间戳 | `std::chrono::steady_clock` / `system_clock` | 超时、统计、定时器 |
| 大结构体按值传参（拷贝） | `std::move` + 移动构造 | `Buffer`、`Packet`、文件元信息 |
| `NULL`、裸虚函数 | `nullptr`、`override` / `final` | 全项目 |
| `#define MAX_X 1024` | `constexpr int kMaxX = 1024;` | 常量 |
| 手写 for 索引循环 | 范围 for + `auto&` | 容器遍历 |
| 手动 `strcpy/sprintf` | `std::string` + `snprintf` | 路径拼接、日志格式化 |

## 补充（来自路线图 9.4）

- `std::make_unique` 属于 C++14，本项目禁用。
- C++11 写法：`std::unique_ptr<T>(new T(...))`。
