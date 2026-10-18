# legacy（只读归档）

这里存放改造前的两个工程，仅用于代码对照与迁移参考，**不参与 CMake 构建**。

- wanpan-dome/：旧网盘客户端（Qt）+ Linux epoll 服务端 + SQL 脚本
- MediaPlayer/：旧 Qt + FFmpeg 播放器（已排除 ffmpeg/SDL 二进制依赖）

二进制依赖（ffmpeg-4.2.2、SDL2-2.0.10、dll）体积较大，未纳入仓库；
如需编译旧播放器，从原目录 D:/mediaplayer/MediaPlayer 取用。
