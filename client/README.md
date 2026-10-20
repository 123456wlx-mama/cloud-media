# cloud-media 客户端（Qt 5.12 / MinGW 32 位）

一句话：**用 Qt 5.12.11 的 mingw73_32 工具链构建 `build-client`，起 Linux 服务端（8004）后跑
`build-client/bin/cloud-media-client.exe --host <IP> --port 8004 --tel <手机号> --password-env CM_CLI_PASSWORD`；
要独立分发就 `scripts/package_client.sh` 出 `deploy/`。**

> 机器特定路径（本机 Qt 安装位置、联调 VM 地址）见 `docs/plans/phase-8.md` 环境段；本文只写通用占位符。

## 构建

~~~bash
export PATH="<QT_ROOT>/Tools/mingw730_32/bin:<QT_ROOT>/5.12.11/mingw73_32/bin:$PATH"
cmake -S . -B build-client -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SERVER=OFF -DBUILD_CLIENT=ON \
  -DCMAKE_PREFIX_PATH=<QT_ROOT>/5.12.11/mingw73_32 \
  -DCMAKE_CXX_COMPILER=<QT_ROOT>/Tools/mingw730_32/bin/g++.exe \
  -DCMAKE_C_COMPILER=<QT_ROOT>/Tools/mingw730_32/bin/gcc.exe
cmake --build build-client -j8
ctest --test-dir build-client --output-on-failure
~~~

播放器依赖（FFmpeg 4.2.2 / SDL 2.0.10）的头文件与导入库已放在 `client/player/deps/`；
运行期 DLL 不入库，用 `scripts/sync_player_dlls.sh` 从 `D:/mediaplayer/MediaPlayer` 复制到
`client/player/deps/dll`（构建后自动拷到 exe 旁）。

## 起服务端

服务端是 Linux 侧（Reactor + MySQL/Redis），本仓库用单独的构建目录：

~~~bash
cmake -S . -B build-on -DBUILD_SERVER=ON -DBUILD_CLIENT=OFF && cmake --build build-on -j8
./build-on/bin/cloud-media-server -c build/e2e.conf     # 监听 8004，配置见 build/e2e.conf
~~~

## 跑客户端

~~~bash
build-client/bin/cloud-media-client.exe --host <server-host> --port 8004 \
  --tel 13900000011 --password-env CM_CLI_PASSWORD
~~~

- 双击视频 = 边下边播；右键 = 下载 / 分享 / 删除；工具栏 = 打开分享 / 回收站 / 刷新。
- 无界面联调：`--smoke`（登录+列表）、`--smoke --upload <file>`、`--play-uf <ufId>`、
  `--play-share <code> --access <code>`。
- 全链路 demo：`scripts/e2e_p8.sh`，结尾 `FULL_LINK_OK`。

## 打包

~~~bash
scripts/package_client.sh --verify   # windeployqt → deploy/，并在干净 PATH 下自检
~~~

`deploy/` 内含 Qt DLL、`platforms/qwindows.dll`、`sqldrivers/qsqlite.dll`、FFmpeg/SDL DLL，
拷到任意机器双击即可运行，不需要 Qt 环境变量。
