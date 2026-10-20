# Phase 9 计划与联调记录

> 范围：P8 收尾后的客户端加固。P9-1 只补两个缺口：**渲染后端可替换** + **音画同步经得起追问**。
> 同步方案的正式文档在 P9-2 写，这里记录实现与回归证据。

## 1. 渲染后端抽象

~~~text
ui/VideoSurface.h        接口：widget() / setImage(QImage) / clear() / backendName()
   ├─ ui/GlVideoSurface      QOpenGLWidget + 纹理上屏（默认）
   └─ ui/PainterVideoSurface QWidget + QPainter::drawImage（无 GL / 远程桌面回退）
ui/VideoSurface.cpp      openGlAvailable() + createVideoSurface(parent, forcePainter)
ui/Letterbox.h           letterboxRect(image, target) —— 纯函数，宽高比与黑边
~~~

- 选择策略：默认探测 `QOpenGLContext::create()`，失败自动回退 QPainter；命令行 `--painter` 可强制回退
  （`MainWindow(net, queue, parent, forcePainter)` → `PlayerPane(..., forcePainter)`）。
- GL 与 Painter 用同一个 `letterboxRect`，保证两种后端画面位置一致；GL 侧把 Qt 的左上原点换算成
  GL 的左下 viewport（`height - box.y - box.height`）。
- 红线不变：两个实现都只吃 `QImage`，`grep libav/SDL client/ui/` 仍然为空。
- 单测：`client_ui_tests`（6 例）覆盖 16:9/4:3/竖屏/同比例/极小目标/非法尺寸。

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

1. `seek()` 在 GUI 线程提前把 `audioClock` 改成目标值，而 demux 线程还没 flush —— 旧帧与新时钟打架，
   seek 风暴里最大漂移 **50.3s**。修法：seek() 只记目标，时钟在 doSeek（flush 之后）重置。
2. 视频线程在 `syncToClock()` 里等时钟时可能又发生一次 seek，等待结束后没复检就交付 ——
   55.4s 的旧帧被渲染在 19.8s 的时钟上。修法：交付前复检 `seekGen` / `seekDropMs` / 落后阈值。
3. 屏障被"目标之后的旧帧"提前解除（向后 seek 时 55s 的帧 >= 目标 4.8s），随后关键帧前的正常帧被放行；
   无音频时更糟：锚点被 seek 前的旧帧锚在 2.1s，画面 4.8s 却按 2.1s 计时。修法：只有确认要渲染的帧
   才解除屏障；无音频的 wall clock 锚点也只在交付前设。

修复后（`scripts/player_regress.sh`，日志 `build-client/regress/`）：

~~~text
SELFTEST_OPEN duration_ms=60024 has_audio=1
SELFTEST[base] pos_ms=2000 drift_ms=4
SELFTEST[seek-storm] seeks=20 max_drift_ms=109 samples=121 dropped=29
SELFTEST[pause] pos_ms=40360 clock_ms=40379
SELFTEST[resume] pos_ms=40360
SELFTEST[pause-resume] pos_ms=41760 drift_ms=12 max_drift_ms=14
SELFTEST[volume-0] frames_since=24 pos_ms=42840
SELFTEST[volume-100] frames_since=25 pos_ms=43840
SELFTEST_DONE frames=274 max_drift_ms=15 dropped=29
SELFTEST_OK

SELFTEST_OPEN duration_ms=60000 has_audio=0        # 纯视频（-an -c copy 生成）
SELFTEST[seek-storm] seeks=20 max_drift_ms=16 dropped=0
SELFTEST[pause-resume] drift_ms=0 max_drift_ms=51
SELFTEST[volume-0/100] frames_since=25/25
SELFTEST_OK
~~~

四条回归：**a) 20 次快进快退**（109ms ≤ 120ms 丢帧阈值，无花屏）、**b) 纯视频无音频**（16ms，系统时钟）、
**c) 暂停→恢复**（14ms）、**d) 音量 0/100**（两种音量下都稳定 25 帧/秒）。

## 3. 截图

- `docs/screenshots/p9-render-gl.png`（默认 GL 后端播放中；启动日志 `player surface: gl`）
- `docs/screenshots/p9-render-painter.png`（`--painter` 强制 QPainter 后端播放中；日志
  `player surface: painter`）

## 4. 命令

~~~bash
# 渲染后端 / 播放器回归
build-client/bin/cloud-media-client.exe --play-local D:/mediaplayer/test.mp4 --screenshot x.png
build-client/bin/cloud-media-client.exe --painter --play-local D:/mediaplayer/test.mp4 --screenshot y.png
scripts/player_regress.sh        # 结尾 PLAYER_REGRESS_OK
~~~

## 5. 遗留

| 项 | 状态 |
|---|---|
| 同步方案正式文档（P9-2） | 待做：把 §2 的阈值、时钟来源、seek 时序画成正式文档 |
| HLS m3u8 侧车 | deferred（P8 已列） |
| 1000 行列表滚动压测 | deferred（P8 已列） |
