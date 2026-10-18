# legacy（只读归档）

两个旧工程已原样复制：wanpan-dome 含 .git 与 .pro.user，MediaPlayer 含 ffmpeg/SDL 二进制与 .pro.user；仅用于代码对照与迁移参考，**不修改、不参与 CMake 构建**。

- wanpan-dome/：旧网盘客户端（Qt）+ Linux epoll 服务端 + SQL 脚本
- MediaPlayer/：旧 Qt + FFmpeg 播放器

注意：wanpan-dome 内含 .git，Git 会将其视为嵌套仓库；不要对 legacy/ 执行 git add/commit。
二进制依赖体积较大，已在根 .gitignore 中排除提交，但本地文件保持原样。
