#!/bin/bash
# 把 32 位 FFmpeg/SDL2 运行期 DLL 复制到 client/player/deps/dll（.gitignore 忽略）。
# 头文件与导入库已在 client/player/deps/{ffmpeg,sdl2}，可入库；DLL 不入库（沿用 legacy 归档原则）。
set -eu
ROOT="${1:-D:/mediaplayer/MediaPlayer}"
DST="$(cd "$(dirname "$0")/.." && pwd)/client/player/deps/dll"
mkdir -p "$DST"
cp -f "$ROOT"/ffmpeg-4.2.2/bin/*.dll "$DST"/
cp -f "$ROOT"/SDL2-2.0.10/SDL2-2.0.10/bin/SDL2.dll "$DST"/
echo "copied $(ls "$DST"/*.dll | wc -l) dlls -> $DST"
