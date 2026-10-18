# Phase 1 收尾实施计划（cloud-media 骨架与工程规范）

**Created:** 2026-10-18
**Status:** In Progress
**Source Spec:** docs/specs/01-架构设计.md 第 10 节；终端改造路线图 Phase 1

## Overview

**Goal:** 在已提交的 Phase 1 骨架上，补齐路线图要求的工程规范、项目上下文、README 架构图与 CI 骨架，并按 DoD 完成一次可复现验收。

**Approach:** 只做文档、规则、CI 与验收，不写业务代码；不改动已提交的目录结构与 CMake target 名。每个任务独立可验证，最后统一提交一次。

**Tech Stack:** CMake 3.16+、C++11、GitHub Actions（ubuntu-latest）、Docker（本地 Linux 冒烟）、Windows MinGW（客户端）。

## Current Baseline

Phase 1 的骨架部分已完成并提交：

- 提交：f2c0d94 chore: 初始化 cloud-media 单仓库骨架与 CMake 分层
- 已有：目录结构、cm_common/cm_protocol、server/client 占位可执行、legacy 归档、.gitignore、README 初版
- 已验证：Windows 下构建客户端成功，输出 cloud-media-client 0.1.0 (protocol v2)；BUILD_SERVER=ON 在 Windows 被明确拒绝
- 尚未完成（本计划范围）：.gitattributes、.codebuddy/rules/cpp11-style.md、CODEBUDDY.md、.codebuddy/commands/*、README 架构图、CI 骨架、Phase 1 完整验收

本计划不重复创建骨架，避免与 f2c0d94 冲突。

## Files and Responsibilities

| 文件/模块 | 动作 | 职责 | 备注 |
|---|---|---|---|
| .gitattributes | Create | 统一文本换行，legacy 保持原样 | 解决 CRLF 警告 |
| .codebuddy/rules/cpp11-style.md | Create | C++11、资源管理、并发、命名、日志规范 | AI 后续改动自动遵守 |
| CODEBUDDY.md | Create | 项目上下文、构建命令、目录与阶段说明 | 新会话入口 |
| .codebuddy/commands/bench.md | Create | 自定义 /bench 命令提示词 | 压测阶段复用 |
| .codebuddy/commands/review-stage.md | Create | 自定义 /review-stage 命令提示词 | 阶段审查 |
| .codebuddy/commands/migrate.md | Create | 自定义 /migrate 命令提示词 | legacy 迁移 |
| README.md | Modify | 架构图、构建矩阵、Phase 1 DoD、文档索引 | 不重复架构文档全文 |
| .github/workflows/ci.yml | Create | Linux 服务端构建 + 冒烟 | Qt 客户端 CI 延后 |
| docs/plans/phase-1.md | Create | 本计划 | 已创建 |

## Assumptions and Dependencies

- Docker Desktop 29.8 可用；本地 Linux 冒烟用 ubuntu:22.04 容器（首次需拉镜像与 apt 包）。
- GitHub Actions 使用 ubuntu-latest；Phase 1 不安装 Qt，只构建服务端占位程序。
- 仓库尚未配置远端；CI 文件先落盘，首次 push 后才会真正运行。
- 本阶段不引入 MySQL/Redis/FFmpeg/Qt 依赖。
- 所有新增文本使用 UTF-8；Windows 下 CMake 生成器为 MinGW Makefiles。
- legacy/wanpan-dome 自带 .gitattributes（*.cpp text eol=lf），会覆盖根目录的 legacy/** -text；换行归一化后必须以内容 diff 为准，不能只看属性。

## Tasks

### Task 1：换行规范与仓库卫生

**Goal:** 增加 .gitattributes，消除 LF/CRLF 歧义，保证 Linux 脚本与 CMake 文件为 LF。

**Files:**
- Create: .gitattributes
- Modify: 无代码文件（仅索引换行归一化）

**Steps:**
- [ ] 创建 .gitattributes，内容如下：

~~~text
* text=auto
*.sh text eol=lf
*.cmake text eol=lf
CMakeLists.txt text eol=lf
*.cpp text eol=lf
*.h text eol=lf
*.md text eol=lf
*.ini text eol=lf
legacy/** -text
~~~

- [ ] 执行换行归一化：

~~~bash
git add .gitattributes
git add --renormalize .
git status --short
~~~

- [ ] 检查 legacy 未被转换、源码被标记为 text：

~~~bash
git check-attr text -- common/base/version.cpp
git check-attr text -- legacy/MediaPlayer/main.cpp
~~~

  - Expected: 第一行输出 text: set；第二行输出 text: unset。

**Security/Boundary Notes:** legacy 是只读归档，禁止因换行归一化改写其内容；wanpan-dome 的嵌套 .gitattributes 优先级高于根规则，归一化后若 git diff 出现 legacy 文件内容变更，立即回退该文件。

**Verification:**
- [ ] git diff --cached --stat 只包含文本文件，不含 legacy 下的源码/二进制。
- [ ] Windows 重新构建客户端仍成功。

---

### Task 2：C++11 项目规范

**Goal:** 把路线图 9.1 的 C++11 要求固化成 AI 可读取的项目规则。

**Files:**
- Create: .codebuddy/rules/cpp11-style.md

**Steps:**
- [ ] 按以下章节编写规则，每条规则写成“必须/禁止 + 示例”：
  - 标准与编译：C++11；-Wall -Wextra -Wpedantic；禁止 C++14/17。
  - 资源管理：RAII；unique_ptr 独占、shared_ptr 共享；禁止裸 new/delete 管理资源；连接/文件/锁必须 RAII。
  - 并发：std::thread/mutex/condition_variable/atomic；状态标志禁止 volatile；禁止忙等。
  - 时间：std::chrono；禁止 gettimeofday 直接调用。
  - 语法：auto、范围 for、nullptr、override/final、=delete、enum class、constexpr、using 别名。
  - 性能：移动语义、emplace_back、避免大对象拷贝。
  - 回调：std::function + lambda，替代函数指针数组。
  - 明确禁用清单：std::make_unique、std::optional、std::string_view、std::filesystem、泛型 lambda。
  - 命名：类 PascalCase；函数/变量 camelCase；成员 m_；常量 k 前缀；表 t_；错误码 ERR_。
  - 日志：格式 [模块] 动作 key=value；禁止记录密码/token；必须带 traceId。
  - 错误处理：统一 ErrorCode；禁止静默失败；错误信息必须含字段名或错误码。
  - 代码审查清单：裸 delete、volatile、memset 结构体、NULL、C++14 特性五项。
- [ ] 在文件开头写明适用范围：common/server/client 全部 C++ 代码；legacy 除外。

**Verification:**
- [ ] 文件存在且包含上述全部章节：

~~~bash
grep -n "禁用清单\|命名\|日志\|错误处理\|审查清单" .codebuddy/rules/cpp11-style.md
~~~

  - Expected: 五个关键词均命中。

**Security/Boundary Notes:** 规范不得放宽 01 架构设计中的依赖红线（common 零 Qt/FFmpeg、server 零 Qt）。

---

### Task 3：项目上下文与自定义命令

**Goal:** 让新会话能凭 CODEBUDDY.md 直接构建与遵守边界，并提供三个常用命令。

**Files:**
- Create: CODEBUDDY.md
- Create: .codebuddy/commands/bench.md
- Create: .codebuddy/commands/review-stage.md
- Create: .codebuddy/commands/migrate.md

**Steps:**
- [ ] CODEBUDDY.md 包含：
  - 一句话定位与当前阶段（Phase 1 收尾）；
  - 目录职责（common/server/client/legacy/docs）；
  - 构建命令：Windows 客户端、Linux 服务端、Docker 冒烟三条；
  - 平台约束：server 仅 Linux；client 仅 Windows；common/protocol 跨平台；
  - 编码约束：链接 .codebuddy/rules/cpp11-style.md；
  - 文档索引：01-架构设计、05-我们做什么、本计划；
  - 只读规则：legacy 不修改、不参与构建；
  - 阶段工作流：设计 → 计划 → 实现 → 验证 → 审查 → 提交。
- [ ] bench.md 内容：运行 benchmarks 下的压测工具，汇总 QPS/P99/吞吐/失败率，写入 docs/benchmark/，并给出前后对比。
- [ ] review-stage.md 内容：审查当前阶段 diff，重点并发安全、资源泄漏、异常分支、C++11 规范、依赖红线。
- [ ] migrate.md 内容：迁移指定 legacy 模块到新目录，先补测试再改代码，保持每一步可编译，不修改 legacy 原文件。

**Verification:**
- [ ] CODEBUDDY.md 中的构建命令与实际 CMake 选项一致：

~~~bash
grep -n "BUILD_SERVER=ON\|BUILD_CLIENT=ON\|MinGW Makefiles" CODEBUDDY.md
~~~

  - Expected: 三条命令均出现。
- [ ] 三个命令文件存在且非空。

**Security/Boundary Notes:** CODEBUDDY.md 不得写入数据库密码、API Key、服务器私钥；配置示例只用占位符。

---

### Task 4：README 架构图与构建矩阵

**Goal:** 让面试官/新成员 5 分钟内看懂仓库分层与构建方式。

**Files:**
- Modify: README.md

**Steps:**
- [ ] 在“当前状态”后加入 mermaid 组件图（使用 GitHub 支持的 mermaid 围栏）：common → protocol → server_core/client_net；player_core → client_ui；server_core → server 可执行；client_ui → client 可执行。
- [ ] 增加构建矩阵表：Windows 客户端（MinGW）、Linux 服务端、Docker 冒烟三行，列出命令与产物路径。
- [ ] 增加 Phase 1 DoD 清单：目录一致、两端可构建、占位程序可输出版本、legacy 不参与构建。
- [ ] 增加文档索引：docs/specs/01-架构设计.md、research/05-我们做什么.md、docs/plans/phase-1.md。
- [ ] 保留现有“目录”小节，补一句 legacy 只读。

**Verification:**
- [ ] README 含 mermaid 与三条构建命令：

~~~bash
grep -n "mermaid\|BUILD_SERVER=ON\|BUILD_CLIENT=ON\|docker run" README.md
~~~

  - Expected: 四个关键词均命中。
- [ ] 文档链接目标文件存在。

---

### Task 5：CI 骨架

**Goal:** 建立最小可用的 Linux CI，保证服务端骨架持续可构建。

**Files:**
- Create: .github/workflows/ci.yml

**Steps:**
- [ ] 创建 workflow，内容如下：

~~~yaml
name: ci
on:
  push:
  pull_request:
jobs:
  linux-server:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Configure
        run: cmake -S . -B build -DBUILD_SERVER=ON -DBUILD_CLIENT=OFF -DBUILD_TESTS=OFF
      - name: Build
        run: cmake --build build -j4
      - name: Smoke
        run: ./build/bin/cloud-media-server --version
~~~

- [ ] 不在 Phase 1 安装 Qt/MySQL/Redis/FFmpeg；不跑 ctest（Phase 2 再加入）。
- [ ] 校验 YAML 可解析：

~~~bash
python -c "import yaml; yaml.safe_load(open('.github/workflows/ci.yml', encoding='utf-8')); print('yaml-ok')"
~~~

  - Expected: 输出 yaml-ok。

**Security/Boundary Notes:** workflow 不引用任何 secrets；actions 使用固定主版本 @v4；后续加缓存时不得缓存第三方凭据。

---

### Task 6：Phase 1 全量验收与提交

**Goal:** 按 01 架构设计第 10 节与路线图 Phase 1 DoD 完成验收，并提交收尾改动。

**Files:**
- Modify: 无新增代码
- Verify: 全仓库

**Steps:**
- [ ] Windows 客户端构建与冒烟：

~~~bash
cmake -S . -B build -G "MinGW Makefiles" -DBUILD_SERVER=OFF -DBUILD_CLIENT=ON -DBUILD_TESTS=OFF
cmake --build build -j8
./build/bin/cloud-media-client.exe --version
~~~

  - Expected: cloud-media-client 0.1.0 (protocol v2)。

- [ ] Linux 服务端 Docker 冒烟：

~~~bash
docker run --rm -v D:/cloud-media:/src -w /src ubuntu:22.04 bash -lc "apt-get update -qq && apt-get install -y -qq cmake g++ && cmake -S . -B build-linux -DBUILD_SERVER=ON -DBUILD_CLIENT=OFF -DBUILD_TESTS=OFF && cmake --build build-linux -j4 && ./build-linux/bin/cloud-media-server --version"
~~~

  - Expected: cloud-media-server 0.1.0 (protocol v2)。若 Docker 无法挂载 D:/cloud-media，改用 //d/cloud-media 重试。

- [ ] 目录一致性检查：

~~~bash
find . -maxdepth 1 -type d -not -path "./.git" -not -path "./build" | sort
~~~

  - Expected: benchmarks、client、cmake、common、deploy、docs、legacy、scripts、server、sql、thirdparty。

- [ ] 依赖红线检查：

~~~bash
grep -RIn "QObject\|QString\|#include <Q" common/ || echo clean-qt
grep -RIn "libav\|SDL_" common/ || echo clean-media
grep -RIn "#include \"server/" client/ || echo clean-server-dep
~~~

  - Expected: 三条均输出 clean-*。

- [ ] 换行与空白检查：

~~~bash
git diff --check
git status --short
~~~

  - Expected: git diff --check 无输出；提交前 status 只包含本阶段预期文件。

- [ ] 提交（路线图要求每阶段一次提交；本次是 Phase 1 骨架提交 f2c0d94 的收尾补充）：

~~~bash
git add -A
git commit -m "chore: 补齐 Phase 1 工程规范与 CI"
~~~

## Verification

- [ ] .gitattributes 生效：源码 text: set，legacy text: unset。
- [ ] .codebuddy/rules/cpp11-style.md 覆盖标准、资源、并发、命名、日志、错误处理、审查清单。
- [ ] CODEBUDDY.md 含三条构建命令、平台约束、legacy 只读规则。
- [ ] 三个 .codebuddy/commands/*.md 非空。
- [ ] README 含 mermaid 架构图、构建矩阵、Phase 1 DoD、文档索引。
- [ ] CI YAML 通过 PyYAML 解析。
- [ ] Windows 客户端与 Docker Linux 服务端均输出版本号。
- [ ] 目录一致性与依赖红线检查通过。
- [ ] git 工作区干净，收尾提交存在。

## Rollback

- 本阶段只新增/修改文档、规则与 CI，不涉及运行时代码与数据；回滚使用 git revert 对应提交。
- .gitattributes 的换行归一化若造成意外 diff，可先移除该文件并执行 git add --renormalize . 回退索引中的换行变更。
- CI 文件可单独删除，不影响本地构建。

## Deferred Decisions

- Windows CI：Phase 1 不引入 Qt/MinGW 安装；客户端持续集成延后到 Phase 8，当前以本地 MinGW 构建为准。
- 远端仓库与分支保护：待确定 Gitee/GitHub 私有仓库后配置；CI 文件先落盘。
- 编译器矩阵：Phase 1 只验证 ubuntu-latest 的默认 g++；多版本矩阵留到 Phase 3/6。
- mermaid 渲染检查：本地无 mermaid CLI，Phase 1 以 GitHub 预览为准；若渲染异常，退回 ASCII 架构图。
- .codebuddy/commands 的高级参数化：先提供固定提示词，后续按实际使用再扩展。
