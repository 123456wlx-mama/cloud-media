# MediaPlayer —— FFmpeg + SDL 的 Qt 视频播放器

一个 **从零手写**的桌面视频播放器：**Qt 做界面，FFmpeg 做解封装/解码，SDL 做音频输出，OpenGL 做画面渲染**。
不使用任何现成播放器框架/组件，线程模型、音视频同步、跳转（seek）、队列与资源回收全部自己实现。

支持**本地文件**与**网络流**（RTMP 直播/点播、HLS `http(s)://...m3u8`），支持播放/暂停/停止、进度拖动、音量调节、总时长与当前时间显示。

> 详细的技术剖析（线程模型、同步原理、seek 流程、音频链路、排错与扩展）见
> **[docs/项目技术说明.md](docs/项目技术说明.md)**。

---

## 1. 目录结构

```
MediaPlayer/
├── MediaPlayer.pro            # qmake 工程文件（内含 FFmpeg / SDL 的 include 与 lib 配置）
├── main.cpp                   # 入口：安装日志钩子 + 支持命令行参数直接播放
├── videoplayer.h/.cpp         # ★ 播放器内核：解封装、音视频解码、同步、seek、状态机
├── playerdialog.h/.cpp/.ui    # 界面：打开/播放/暂停/停止、进度条、音量、网络流地址栏
├── PacketQueue.h/.cpp         # 自实现的线程安全 AVPacket 队列（SDL 互斥锁 + 条件变量）
├── videoslider.h/.cpp         # 自定义进度条：点击任意位置直接跳转
├── glrender/                  # OpenGL 渲染控件
│   ├── glvideowidget.h/.cpp   #   QOpenGLWidget + 着色器 + 纹理（等比缩放居中）
│   └── glrender.pri           #   子工程引入文件
├── ffmpeg-4.2.2/              # FFmpeg 头文件与导入库（include/ lib/）
├── SDL2-2.0.10/               # SDL2 头文件与导入库（include/ lib/x86/）
└── dll/                       # 运行期 DLL（avcodec-58 等 8 个 + SDL2.dll + ffmpeg.exe）
```

构建产物目录（仓库外）：`D:/mediaplayer/build-MediaPlayer-Desktop_Qt_5_12_11_MinGW_32_bit-Debug/debug/MediaPlayer.exe`

---

## 2. 技术栈

| 分层 | 技术 |
|---|---|
| 语言 / 标准 | C++11（`CONFIG += c++11`），MinGW 7.3.0 **32-bit** |
| 界面 | Qt 5.12.11（QtWidgets / QOpenGLWidget / 信号槽 / .ui + moc） |
| 解封装 / 解码 | **FFmpeg 4.2.2**：`avformat`、`avcodec`、`swscale`（YUV→RGB32）、`swresample`（音频重采样） |
| 音频输出 | **SDL 2.0.10**：`SDL_OpenAudioDevice` + 音频回调（`SDL_MixAudioFormat`） |
| 线程 | `QThread`（读线程）+ `SDL_CreateThread`（视频解码线程）+ **SDL 音频回调线程** |
| 同步原语 | SDL `mutex` / `cond`（PacketQueue）、标志位状态机 |
| 画面渲染 | OpenGL（着色器 + 纹理，GPU 缩放） |
| 构建 | qmake + make，Qt Creator（`Desktop Qt 5.12.11 MinGW 32-bit`） |

---

## 3. 功能清单

| 功能 | 说明 |
|---|---|
| 打开本地文件 | 按钮选择文件，支持常见格式（mp4/mkv/avi/flv/mp3…取决于 FFmpeg 编解码器） |
| 打开网络流 | 地址栏填写 `rtmp://` / `http(s)://...m3u8` 等，点"打开网络流" |
| 命令行直接播放 | `MediaPlayer.exe <文件路径或URL>` |
| 播放 / 暂停 / 停止 | 暂停时音频静音、视频线程挂起；停止会等待线程退出并回收资源 |
| 进度拖动（seek） | 点击进度条任意位置跳转，含关键帧对齐与解码器缓存清空（不花屏） |
| 音量调节 | 0~128（`SDL_MIX_MAXVOLUME`），由 `SDL_MixAudioFormat` 混音时生效 |
| 时长 / 当前时间 | 总时长在打开时获取；当前时间由定时器刷新 |
| 纯音频 / 纯视频 | 没有视频流只播声音，没有音频流用系统时钟同步只播画面 |
| OpenGL 渲染 | 等比缩放居中，窗口可自由拉伸，画质优于 QLabel 软件缩放 |
| 播放结束 | 自动清屏为黑屏并回到停止态 |

---

## 4. 编译与运行

### 4.1 编译

1. Qt Creator 打开 `MediaPlayer/MediaPlayer.pro`；
2. 构建套件选 **Desktop Qt 5.12.11 MinGW 32-bit**（Debug 或 Release 均可）；
3. 构建 → 运行。

`.pro` 中已配置好依赖路径（无需系统环境变量）：

```qmake
INCLUDEPATH += $$PWD/ffmpeg-4.2.2/include  $$PWD/SDL2-2.0.10/include
LIBS += $$PWD/ffmpeg-4.2.2/lib/avcodec.lib ... $$PWD/SDL2-2.0.10/lib/x86/SDL2.lib
include($$PWD/glrender/glrender.pri)
```

> 注意是 **32 位**工程（FFmpeg/SDL 均为 x86 版本），用 64 位套件链接会失败。
> SDL 目录若解压多嵌套一层，`.pro` 会自动适配（`exists($$SDL_DIR/SDL2-2.0.10/include/SDL.h)`）。

### 4.2 部署运行（把程序拷到别的机器）

把 `dll/` 下的 DLL 拷到 exe **同级目录**：

```
avcodec-58.dll  avformat-58.dll  avutil-56.dll  swscale-5.dll
swresample-3.dll  avfilter-7.dll  avdevice-58.dll  postproc-55.dll
SDL2.dll
```

Qt 程序还需：`Qt5Cored.dll`、`Qt5Guid.dll`、`Qt5Widgetsd.dll`（Release 版不带 `d`）、
MinGW 运行库（`libgcc_s_dw2-1.dll`、`libstdc++-6.dll`、`libwinpthread-1.dll`）以及 `platforms/qwindows.dll`。
可用 Qt 自带工具一键部署：

```bat
windeployqt MediaPlayer.exe
```

### 4.3 快速验证

```bat
:: 用仓库自带的测试视频（60 秒 testsrc + 正弦音）
MediaPlayer.exe D:\mediaplayer\test.mp4
```

仓库根目录另有两个 PowerShell 自动化脚本：`autotest.ps1`（UIA 驱动的 UI 自动化）、`verify_play.ps1`（播放校验，输出 `verify_shot.png` 截图）。
运行期日志写在 `D:/mediaplayer/player_debug.log`（`main.cpp` 里 `qInstallMessageHandler` 重定向）。

---

## 5. 界面说明

| 区域 | 控件 | 作用 |
|---|---|---|
| 画面区 | `wdg_glShow`（GLVideoWidget） | OpenGL 视频渲染，黑色背景 |
| 进度区 | `lb_currentTime` / `slider_videoProcess` / `lb_totalTime` | 当前时间、可点击跳转的进度条、总时长 |
| 网络区 | `le_url` + `pb_openUrl` | 填写并打开 RTMP/HLS 地址 |
| 控制区 | `pb_open` / `pb_play` / `pb_pause` / `pb_stop` | 打开文件、播放、暂停、停止 |
| 音量区 | `slider_volume` | 音量 0~128 |
| 信息区 | `lb_videoName` | 当前播放文件名 |

---

## 6. 实现亮点（踩过的坑）

| 问题 | 解决方式 |
|---|---|
| **中文路径打不开** | FFmpeg 在 Windows 要 UTF-8 路径，`toStdString()` 是 GBK → 改用 `toUtf8()` |
| **网络流打不开时界面卡死** | 给 `AVFormatContext` 设 `interrupt_callback`，5 秒连不上或点了停止就打断阻塞 |
| **跳转后花屏** | seek 后清空队列并放入 `FLUSH` 特殊包，解码线程取到后 `avcodec_flush_buffers()` |
| **向左（快退）跳转卡死** | 跳转时把 `video_clock` 归零，避免视频帧一直等旧音频时钟 |
| **QLabel 显示糊、窗口拉大后缩不回去** | 自写 `GLVideoWidget`：OpenGL 纹理 + 着色器，GPU 缩放、等比居中、可自由伸缩 |
| **音视频不同步** | 以**音频时钟为主时钟**，视频帧超前就延时等待；无音轨时退化为系统时钟同步 |
| **一次性把文件读完导致内存暴涨** | 队列字节数超阈值（`MAX_AUDIO_SIZE` / `MAX_VIDEO_SIZE`）时暂停读线程 |
| **退出崩溃 / 卡死** | 统一 `quit` 标志 + `packet_queue_abort()` 唤醒阻塞线程 + 按序回收（先停解码线程再释放解码器） |
| **HLS 频繁拖动打爆服务端** | 界面层用 `QElapsedTimer` 做跳转节流 |
| **非 POD 结构体被 `memset` 清零** | `VideoState` 改为构造函数逐个初始化成员 |

---

## 7. 常见问题

| 现象 | 原因 / 解决 |
|---|---|
| 启动报缺 `avcodec-58.dll` / `SDL2.dll` | 把 `dll/` 下所有 DLL 拷到 exe 同级目录 |
| 报 `no Qt platform plugin` | 缺 `platforms/qwindows.dll`（必须是 exe 同级下的 `platforms` 子目录） |
| 打开本地文件失败 | 路径含中文已处理；若仍失败看 `player_debug.log` 中 `can't open file` |
| 打开 RTMP/HLS 长时间无响应后失败 | 5 秒超时保护生效：检查网络、地址、服务端是否已推流 |
| 有画面没声音 | 源文件无音轨，或 SDL 音频设备打开失败（看日志 `Couldn't init SDL`） |
| 有声音没画面 | 无视频流/视频解码器缺失，程序会降级为只播声音 |
| 拖动后画面短暂花屏 | 已用 FLUSH 包处理；若仍出现，多为源关键帧稀疏（GOP 过大） |

---

## 8. 后续可扩展

- 硬解码（DXVA / CUDA）降低 CPU；
- 播放列表、倍速播放、截图保存；
- 字幕（libass）与音轨切换；
- 更精准的 seek（解码到目标时间戳而非关键帧）；
- 视频滤镜（libavfilter：裁剪、旋转、调色）。
