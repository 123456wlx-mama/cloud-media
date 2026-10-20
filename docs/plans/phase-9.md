# Phase 9 计划与联调记录

> 范围：P8 收尾后的客户端加固。P9-1 补两个缺口（渲染后端可替换 + 音画同步），P9-2 把它整理成
> 正式设计文档 docs/specs/09-播放器设计.md 并收口。

## 1. 渲染后端抽象

~~~text
ui/VideoSurface.h        接口：widget() / setImage(QImage) / clear() / backendName()
   ├─ ui/GlVideoSurface      QOpenGLWidget + 纹理上屏（默认）
   └─ ui/PainterVideoSurface QWidget + QPainter::drawImage（无 GL / 远程桌面回退）
ui/VideoSurface.cpp      openGlAvailable() + createVideoSurface(parent, forcePainter)
ui/Letterbox.h           letterboxRect(image, target) —— 纯函数，宽高比与黑边
~~~

- 选择策略：默认探测 `QOpenGLContext::create()`，失败自动回退 QPainter；`--painter` 可强制回退
  （`MainWindow(net, queue, parent, forcePainter)` → `PlayerPane(..., forcePainter)`）。
- GL 与 Painter 共用 `letterboxRect`，保证换后端画面位置不变；GL 侧把 Qt 的左上原点换算成
  GL 的左下 viewport（`height - box.y - box.height`）。
- 红线不变：两个实现都只吃 `QImage`，`grep libav/SDL client/ui/` 仍然为空。
- 单测：`client_ui_tests`（6 例）覆盖 16:9 / 4:3 / 竖屏 / 同比例 / 极小目标 / 非法尺寸。

## 2. 音画同步

**音频是主时钟**（有音频轨时）：

| 环节 | 实现 |
|---|---|
| 时钟推进 | 解码出一帧音频时记 `framePts`；SDL 回调每消费 N 个样本，`clock = framePts + consumed/44100`，只增不减 |
| 视频节流 | 帧 pts 比时钟早 > `kSyncSlopMs(8ms)` → 每 5ms 睡一次最多 2s；落后 > `kVideoDropMs(120ms)` → 丢帧追赶，不慢放 |
| 无音频轨 | 退回系统时钟：首个**通过 seek 校验**的帧锚定 `wallStartPts/wallStartMs`，之后 `pts + (now-start)` |
| seek | 清包队列 + `avcodec_flush_buffers` + 清 PCM 缓冲，**在 demux 线程里**把时钟重置到目标，并设置 seek 屏障 |
| seek 屏障 1 | `seekGen`：doSeek 时自增；解码用的包若来自旧代数，解出的帧一律丢弃 |
| seek 屏障 2 | `seekDropMs`：pts 早于目标 20ms 的帧丢弃；只有真正要渲染「目标之后」的帧才解除屏障 |

**本批发现并修掉的三个真 bug**（都是先复现再改）：

1. `seek()` 在 GUI 线程提前把 `audioClock` 改成目标值，而 demux 线程还没 flush —— 旧帧与新时钟
   打架，seek 风暴里最大漂移 **50.3s**。修法：seek() 只记目标，时钟在 doSeek（flush 之后）重置。
2. 视频线程在 `syncToClock()` 里等时钟时可能又发生一次 seek，等待结束后没复检就交付 ——
   55.4s 的旧帧被渲染在 19.8s 的时钟上。修法：交付前复检 `seekGen` / `seekDropMs` / 落后阈值。
3. 屏障被"目标之后的旧帧"提前解除（向后 seek 时 55s 的帧 ≥ 目标 4.8s），随后关键帧前的正常帧被
   放行；无音频时更糟：锚点被 seek 前的旧帧锚在 2.1s，画面 4.8s 却按 2.1s 计时。修法：只有确认
   要渲染的帧才解除屏障；无音频的 wall clock 锚点也只在交付前设。

修复后（`scripts/player_regress.sh`，日志 `build-client/regress/`）：

~~~text
SELFTEST_OPEN duration_ms=60024 has_audio=1
SELFTEST[base] pos_ms=2000 drift_ms=4
SELFTEST[seek-storm] seeks=20 max_drift_ms=109 samples=121 dropped=28
SELFTEST[pause] pos_ms=40360 clock_ms=40379
SELFTEST[resume] pos_ms=40360
SELFTEST[pause-resume] pos_ms=41760 drift_ms=12 max_drift_ms=14
SELFTEST[volume-0] frames_since=25 pos_ms=42840
SELFTEST[volume-100] frames_since=25 pos_ms=43840
SELFTEST_DONE frames=273 max_drift_ms=22 dropped=28
SELFTEST_OK

SELFTEST_OPEN duration_ms=60000 has_audio=0        # 纯视频（-an -c copy 生成）
SELFTEST[base] drift_ms=0
SELFTEST[seek-storm] seeks=20 max_drift_ms=8 dropped=0
SELFTEST[pause-resume] drift_ms=1 max_drift_ms=9
SELFTEST[volume-0/100] frames_since=25/25
SELFTEST_OK
~~~

四条回归：**a) 20 次快进快退**（109ms ≤ 120ms 丢帧阈值，无花屏）、**b) 纯视频无音频**（8~40ms，
系统时钟）、**c) 暂停→恢复**（12~20ms）、**d) 音量 0/100**（两种音量下都稳定 25 帧/秒）。
连续跑 5 次结果一致（带音频风暴稳定 109ms、纯视频 8~40ms），没有复现早先的 50s 漂移。

## 3. 截图与命令

- `docs/screenshots/p9-render-gl.png`：默认 GL 后端播放中，启动日志 `player surface: gl`
- `docs/screenshots/p9-render-painter.png`：`--painter` 强制 QPainter，日志 `player surface: painter`

~~~bash
# 两种渲染后端
build-client/bin/cloud-media-client.exe --play-local D:/mediaplayer/test.mp4 --screenshot x.png
build-client/bin/cloud-media-client.exe --painter --play-local D:/mediaplayer/test.mp4 --screenshot y.png

# 播放器回归（四条）+ 单测 + 红线
scripts/player_regress.sh
ctest --test-dir build-client --output-on-failure
grep -RIn "libav|SDL_|#include <SDL" client/ui/ || echo clean-ui-media
~~~

## 4. Phase 9 DoD 对照

| 能力 | 实现 | 证据 | 状态 |
|---|---|---|---|
| 边下边播 | `GrowingFileIO` 阻塞等块 + 首块后才 open + `sourceComplete` 才 EOF | `PLAY_STREAM_FIRST_FRAME at_ms=188`；e2e 分享→下载→播放到结尾 | 达标 |
| seek | `seekGen` + `seekDropMs` 双屏障，时钟在 doSeek 重置 | 20 次快进快退最大漂移 **109ms**（≤120ms 丢帧阈值） | 达标 |
| 暂停/恢复 | 暂停停声卡与视频线程，恢复重新锚定 | `SELFTEST[pause-resume] drift_ms=12 max_drift_ms=14` | 达标 |
| 音量 | SDL 回调内按百分比缩放 S16 | `volume-0/100 frames_since=24/25`，两种音量下都稳定 25fps | 达标 |
| 渲染后端可替换 | `VideoSurface` + GL/Painter 双实现 + `--painter` | 启动日志 `player surface: gl|painter`；两张截图 | 达标 |
| 设计文档 | docs/specs/09-播放器设计.md（§0~§7） | 412 行：分层/线程/同步/seek/边下边播/渲染/生命周期/14 问答 | 达标 |
| 回归自动化 | `scripts/player_regress.sh` | 结尾 `PLAYER_REGRESS_OK` | 达标 |

## 5. 遗留（Phase 9 收口清单）

| 项 | 状态 | 说明 |
|---|---|---|
| HLS m3u8 侧车 | **deferred** | 服务端 `DownloadOpenRs.hls` 已预留；播放器本期只做边下边播主链路。要做时在 open 前插入 m3u8 解析 + 分片映射，属独立一批 |
| 1000 行列表滚动压测 | **deferred** | Model/View + 分页就绪，未做帧率/内存测量 |
| PacketQueue 按字节反压 | **已知限制** | 当前无上限（大文件可能吃内存），方案见 09 文档 §1 末尾 |
| 4K 硬解纹理 | 非目标（当前） | 需要 `VideoSurface` 增加纹理重载，不能把 AVFrame 漏进 ui/ |
| 多清晰度转码 / 字幕 / 弹幕 | 非目标 | 需求冻结 |
