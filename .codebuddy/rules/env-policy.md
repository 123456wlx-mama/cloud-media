# 环境与命令策略（强制）

> 适用范围：本仓库所有后续会话、子代理、后台任务。
> 生效日期：2026-10-18
> 优先级：高于项目内其他文档中的命令示例；冲突时以本文件为准。

## 1. 禁止任何联网命令

禁止执行任何需要网络的命令或构建行为，包括但不限于：

- docker pull / docker run / docker build / docker compose pull
- git clone / git fetch / git pull / git submodule update
- CMake FetchContent / ExternalProject / 任何会触发下载的依赖获取
- apt / apt-get / pip / npm / yarn / pnpm install
- curl / wget 下载依赖
- 任何访问 GitHub、Gitee、镜像站或其他外部仓库的行为

## 2. 禁止修改 TLS / 证书 / 安全设置

禁止：

- 关闭证书校验（如 apt 的 Acquire::https::Verify-Peer false、git 的 http.sslVerify false、curl -k、NODE_TLS_REJECT_UNAUTHORIZED=0）
- 修改系统 CA、证书库、代理、防火墙、hosts、DNS
- 修改 Docker daemon 配置或镜像源
- 任何降低安全性的临时绕过

## 3. 构建与测试只输出命令，不执行

- 禁止执行 cmake、ctest、编译器、链接器、测试可执行文件、benchmark、docker 等构建/测试命令。
- 需要验证时，只输出完整命令文本，由用户手工运行；等待用户反馈结果。
- 不得用后台任务或脚本代替用户执行。
- 允许读文件、静态分析、编辑源码/文档；不得运行构建产物。

## 4. 第三方依赖

- 需要第三方库时立即停止，说明库名、版本、用途、放置路径，由用户安装到 thirdparty/。
- 不得自行下载、解压、克隆或调用包管理器。
- 现有 FetchContent（GTest）视为待整改：后续构建前必须改为本地 thirdparty/googletest 或用户提供的系统库；未整改前不得执行构建。

## 5. 例外

- 仅当用户在当前消息中逐条明确授权某个具体命令时，才可执行该条；不得推断、批量授权或延续到后续会话。
- 若用户要求与上述规则冲突，先指出冲突并等待确认。

## 6. 违规处理

- 发现即将执行违规命令时，停止并说明原因，给出用户可手工执行的命令文本。

## 7. 协议 CRC 与字节序硬规则

1. **CRC 必须使用流式 cm::Crc32，且只覆盖 version + type + len + seq 共 11 字节 + payload。**
   - 计算范围：header 偏移 4 开始的 11 字节（version 1 + type 2 + len 4 + seq 4），再 update payload。
   - 绝不允许用一次性 crc32 覆盖整个 frame，也绝不允许把 offset 15~18 的 CRC 字段算进去；否则编码写回 CRC 后，解码侧必然 BadCrc。
   - 该问题已修复两次，不得回退；改 Codec 时必须保留正常包/错 CRC 用例。
2. **字节序函数在 cm 命名空间，不在 cm::protocol。**
   - writeU16BE/readU16BE、writeU32BE/readU32BE、writeU64BE/readU64BE 定义于 cm/base/byte_order.h。
   - 调用时写 cm::writeU32BE / cm::readU32BE（或 using 后使用），不得在 cm::protocol 中重复定义同名函数。

## Linux 目标环境约束（Ubuntu 16.04 / g++ 5.4 / cmake 3.5）
1. 所有 CMakeLists.txt 的 cmake_minimum_required 不得高于 3.5。
2. 不得使用 cmake 3.6+ 才有的特性/命令：target_link_options、add_compile_definitions、include(GoogleTest)、gtest_discover_tests、cmake_path 等。测试一律用 add_test 注册。
3. C++ 标准必须显式 -std=c++11（g++ 5.4 默认按 gnu++98 编译，不指定会全量报错）。
4. 可执行文件统一输出到 build/bin（项目已设置 CMAKE_RUNTIME_OUTPUT_DIRECTORY=bin），新增 target 不要另设输出目录。
5. std::string 取 data()/&s[0] 后若再 append/resize，指针可能失效，必须重新取址后再使用（曾因该 UB 导致 Windows 通过、Linux 崩溃）。
