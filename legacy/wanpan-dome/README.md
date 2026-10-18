# Cloud Disk 1.0 —— 私有云网盘（C++ / Qt 全栈）

一个从零手写的私有网盘系统：**Qt 图形客户端 + 两个可互换的服务端实现（Linux epoll / Qt 控制台）+ MySQL & SQLite 双数据库**。
不使用任何现成的网盘框架，网络协议、文件传输、断点续传、秒传、分享等全部自己实现。

> 详细的技术剖析、功能清单、数据库设计、协议表与二次开发指引见 **[docs/项目技术说明.md](docs/项目技术说明.md)**。

---

## 1. 仓库结构

```
cloud-disk-1.0/
├── client/
│   └── NetDesk/            # Qt 5.12.11 图形客户端（工程主体）
│       ├── ckernel.*       # 业务核心（单例 + 协议分发 + 上传下载状态机）
│       ├── logindialog.*   # 登录 / 注册窗口
│       ├── maindialog.*    # 网盘主窗口（文件列表 / 传输列表 / 分享）
│       ├── netapi/         # 网络层：INet + 中介者（TCP 客户端 / TCP 服务端 / UDP）
│       ├── sqlapi/         # 数据库层：SQLite 封装（CSqlite）
│       ├── md5/            # 自实现 MD5（密码加盐、文件秒传校验）
│       ├── images/ tb/ face/   # UI 图片资源
│       └── NetDesk.pro     # qmake 工程文件
├── server/
│   ├── linux-epoll/        # 【主服务端】Ubuntu + epoll + 线程池 + MySQL C API
│   │   ├── src/            # TCPKernel / block_epoll_net / Thread_pool / clogic / Mysql
│   │   ├── include/        # 头文件
│   │   ├── src/makefile    # 手写 makefile
│   │   └── NetDesk.pro     # Qt Creator 工程文件（CONFIG -= qt，纯 C++ 控制台）
│   └── qt-server/          # 【备选服务端】Qt 控制台版，复用客户端 netapi/sqlapi，SQLite 存用户
├── demo/
│   └── Sqldemo/            # 开发过程中的 SQLite 读写 + 断点续传任务缓存练手 Demo
├── sql/
│   └── netdisk_mysql.sql   # 服务端 MySQL 建库脚本（表 / 触发器 / 视图）
└── docs/
    └── 项目技术说明.md      # ★ 项目介绍书（技术栈 / 功能 / 数据库 / 代码工具）
```

## 2. 技术栈速览

| 分层 | 技术 |
|---|---|
| 语言标准 | C++11（`CONFIG += c++11` / `-std=gnu++11`） |
| 客户端框架 | Qt 5.12.11 MinGW 32-bit：QtWidgets、QtNetwork、QtSql、qmake、信号槽、.ui + moc |
| 主服务端 | Linux C++，**epoll(LT) + EPOLLONESHOT + 自研线程池**，MySQL C API（libmysqlclient） |
| 备选服务端 | Qt Core/Network/Sql（`QCoreApplication`），SQLite |
| 数据库 | 服务端 **MySQL(NetDisk)**；客户端 **SQLite(每用户一个 .db)** |
| 网络协议 | 私有二进制协议，TCP，4 字节长度前缀粘包处理，协议号 10000~10029 |
| 构建工具 | qmake、make（手写 makefile）、Qt Creator、g++/MinGW 7.3 |

## 3. 快速开始

### 3.1 服务端（Ubuntu 16.04）

```bash
sudo apt install libmysqlclient-dev mysql-server
mysql -uroot -p < sql/netdisk_mysql.sql        # 建库建表

cd server/linux-epoll/src
make                                            # 依赖 include/ 下的头文件
./server 8004                                   # 默认 8000，客户端配置为 8004

# 服务端文件存储根目录（代码中写死，可按需修改）
#   clogic.cpp: #define DEF_PATH "/home/colin/NetDisk/"
```

数据库连接信息写死在 `include/packdef.h`：

```c
#define _DEF_DB_NAME "NetDisk"
#define _DEF_DB_IP   "localhost"
#define _DEF_DB_USER "root"
#define _DEF_DB_PWD  "335566"
```

### 3.2 客户端（Windows + Qt 5.12.11 MinGW）

1. Qt Creator 打开 `client/NetDesk/NetDesk.pro`，直接构建运行；
2. exe 同级目录的 `config.ini` 决定服务器地址：

```ini
[net]
ip=192.168.142.129
port=8004
```

3. 运行后先注册（手机号 + 密码 + 昵称），再登录；本地文件落盘到 exe 同级的 `NetDisk/`，断点续传任务缓存在 `database/<userid>.db`。

## 4. 主要功能

注册登录、目录浏览与新建文件夹、**文件/文件夹递归上传**、**秒传（MD5 去重）**、**上传/下载断点续传与暂停恢复**、文件/文件夹下载、分享（9 位分享码）与获取分享、批量删除、传输进度展示、任务持久化（重启客户端后任务不丢）。

## 5. 已知限制 / 待改进

- 服务端 SQL 采用 `sprintf` 拼接，无预编译防注入；协议无鉴权 token，登录态仅靠 `userid`。
- 上传/下载为单文件单 timestamp 串行流，服务端用 `userid*100000000 + timestamp` 作 key 放在内存 map 中，重启即丢。
- 分享仅实现了分享码与分享列表，分享密码（`s_linkPassword`）字段预留未实现。
- 客户端业务大量逻辑跑在 GUI 线程（用 `processEvents` 避免卡死），未做真正的多线程传输。
- 路径拼接使用字符串拼接，未做 `..` 之类的路径穿越校验。

详见 `docs/项目技术说明.md` 第 10 节。
